// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth) {
    using namespace Vulkan;
    const bool requested = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                           AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    // SHADPS4_FORCE_ANISO=<n> (16 when left out, 0 or 1: as the title asks): every sampler that
    // filters smoothly between mip levels, or asks for some anisotropic filtering already, gets
    // up to n samples along a texture seen at a slant. Not depth comparisons (shadow maps).
    static const float force_aniso = [] {
        const char* value = std::getenv("SHADPS4_FORCE_ANISO");
        const float n = value != nullptr ? static_cast<float>(std::atof(value)) : 16.0f;
        if (n > 1.0f) {
            LOG_INFO(Render_Vulkan, "Anisotropic filtering raised to {}x on mipmapped textures", n);
        }
        return n;
    }();
    const bool mipmapped = sampler.xy_min_filter != AmdGpu::Filter::Point &&
                           sampler.mip_filter != AmdGpu::MipFilter::None && sampler.MaxLod() > 0.0f;
    const bool forced = force_aniso > 1.0f && !is_depth &&
                        sampler.depth_compare_func == AmdGpu::DepthCompare::Never &&
                        (requested || mipmapped);
    const bool anisotropy_enable =
        instance.IsAnisotropicFilteringSupported() && (requested || forced);
    const float wanted = forced ? std::max(sampler.MaxAniso(), force_aniso) : sampler.MaxAniso();
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(wanted, 1.0f, instance.MaxSamplerAnisotropy()) : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::min(sampler.LodBias(), instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore

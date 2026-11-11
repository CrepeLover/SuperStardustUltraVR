// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/debug.h"
#include "common/memory_patcher.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/wait_stats.h"
#include "core/memory.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

// SHADPS4_LOG_PASSES=1 (or the number of seconds between reports; 1 means 5): every few seconds one
// block of lines, one per distinct kind of draw or dispatch the title issued in that time, the
// busiest first: the colour and depth targets (size, format, guest address), the viewport and
// scissor rectangles, and the render targets the draws read back as textures. It shows the
// resolution a title really draws its scene at, which can differ from the size of its targets.
namespace PassLog {

static u32 Interval() {
    static const u32 seconds = [] {
        const char* value = std::getenv("SHADPS4_LOG_PASSES");
        if (value == nullptr || *value == '\0') {
            return 0u;
        }
        const int n = std::atoi(value);
        return n <= 0 ? 0u : n == 1 ? 5u : u32(n);
    }();
    return seconds;
}

struct Counts {
    u64 calls = 0;
    u64 indices = 0;
};

static std::mutex mutex;
static std::unordered_map<std::string, Counts> counts;
static std::chrono::steady_clock::time_point window_start{};

static std::string ImageText(const VideoCore::Image& image) {
    std::string text = fmt::format("{}x{}", image.info.size.width, image.info.size.height);
    if (image.info.resources.layers > 1) {
        text += fmt::format("x{}", image.info.resources.layers);
    }
    text += ' ';
    text += vk::to_string(image.info.pixel_format);
    if (image.info.num_samples > 1) {
        text += fmt::format(" s{}", image.info.num_samples);
    }
    text += fmt::format(" @{:#x}", image.info.guest_address);
    return text;
}

// Viewport 0 and the scissor rectangle, worked out as UpdateViewportScissorState does.
static std::string ViewportText(const AmdGpu::Regs& regs) {
    std::string text;
    const auto& vp = regs.viewports[0];
    const auto& vp_ctl = regs.viewport_control;
    if (regs.IsClipDisabled()) {
        text = "viewport none (screen-space vertices)";
    } else if (vp.xscale == 0) {
        text = "viewport unset";
    } else {
        const float xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
        const float xscale = std::abs(vp_ctl.xscale_enable ? vp.xscale : 1.f);
        const float yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
        const float yscale = std::abs(vp_ctl.yscale_enable ? vp.yscale : 1.f);
        text = fmt::format("viewport {:g},{:g} {:g}x{:g}", xoffset - xscale, yoffset - yscale,
                           xscale * 2.0f, yscale * 2.0f);
    }
    const auto tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;
    AmdGpu::Scissor scsr{};
    scsr.top_left_x = tl(regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
                         s16(regs.generic_scissor.top_left_x),
                         enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = tl(regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
                         s16(regs.generic_scissor.top_left_y),
                         enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = br(regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
                             regs.generic_scissor.bottom_right_x,
                             enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = br(regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
                             regs.generic_scissor.bottom_right_y,
                             enable_offset ? regs.window_offset.window_y_offset : 0);
    if (regs.mode_control.vport_scissor_enable) {
        scsr.top_left_x = std::max(scsr.top_left_x, s16(regs.viewport_scissors[0].top_left_x));
        scsr.top_left_y = std::max(scsr.top_left_y, s16(regs.viewport_scissors[0].top_left_y));
        scsr.bottom_right_x = s16(std::min<u32>(AmdGpu::Scissor::Clamp(scsr.bottom_right_x),
                                                regs.viewport_scissors[0].bottom_right_x));
        scsr.bottom_right_y = s16(std::min<u32>(AmdGpu::Scissor::Clamp(scsr.bottom_right_y),
                                                regs.viewport_scissors[0].bottom_right_y));
    }
    text += fmt::format(", scissor {},{} {}x{}", AmdGpu::Scissor::Clamp(scsr.top_left_x),
                        AmdGpu::Scissor::Clamp(scsr.top_left_y), scsr.GetWidth(), scsr.GetHeight());
    return text;
}

static void Note(std::string key, u64 indices) {
    std::scoped_lock lock{mutex};
    auto& entry = counts[std::move(key)];
    entry.calls++;
    entry.indices += indices;
    const auto now = std::chrono::steady_clock::now();
    if (window_start == std::chrono::steady_clock::time_point{}) {
        window_start = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - window_start).count();
    if (elapsed < double(Interval())) {
        return;
    }
    std::vector<std::pair<std::string, Counts>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.calls > b.second.calls; });
    LOG_INFO(Render_Vulkan, "Passes in the last {:.1f} s ({} kinds, busiest first):", elapsed,
             sorted.size());
    // With shader hashes (SHADPS4_LOG_PASSES_SHADERS) there are many more kinds; list them all.
    static const size_t MaxLines = [] {
        const char* value = std::getenv("SHADPS4_LOG_PASSES_SHADERS");
        return value != nullptr && *value != '\0' && *value != '0' ? size_t(1000) : size_t(48);
    }();
    for (size_t i = 0; i < std::min(sorted.size(), MaxLines); ++i) {
        LOG_INFO(Render_Vulkan, "  {:>7} x {:>10} indices: {}", sorted[i].second.calls,
                 sorted[i].second.indices, sorted[i].first);
    }
    if (sorted.size() > MaxLines) {
        LOG_INFO(Render_Vulkan, "  ... {} more kinds", sorted.size() - MaxLines);
    }
    counts.clear();
    window_start = now;
}

// SHADPS4_LOG_PASSES_READS=all also lists the plain (not block-compressed) textures a pass samples,
// not only the render targets: a texture the cache keeps apart from the render target at the same
// address shows up as a "(texture)" there.
static bool AllReads() {
    static const bool all = [] {
        const char* value = std::getenv("SHADPS4_LOG_PASSES_READS");
        return value != nullptr && std::string(value) == "all";
    }();
    return all;
}

// The render targets among the images a draw or dispatch has bound, other than its own targets.
static std::string ReadTargetsText(VideoCore::TextureCache& texture_cache,
                                   std::span<const VideoCore::ImageId> bound,
                                   std::span<const VideoCore::ImageId> own, bool mark_storage) {
    std::vector<std::string> names;
    for (const auto id : bound) {
        if (!id || std::find(own.begin(), own.end(), id) != own.end()) {
            continue;
        }
        const auto& image = texture_cache.GetImage(id);
        const bool storage = mark_storage && image.usage.storage;
        const bool target = image.usage.render_target || image.usage.depth_target;
        const bool plain_texture = AllReads() && !image.info.props.is_block;
        if (!target && !storage && !plain_texture) {
            continue;
        }
        std::string name = ImageText(image);
        if (storage) {
            name += " (storage)";
        } else if (!target) {
            name += " (texture)";
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end());
    std::string text;
    for (const auto& name : names) {
        text += text.empty() ? "" : "; ";
        text += name;
    }
    return text;
}

// SHADPS4_LOG_PASSES_SHADERS=1 adds each draw's shader hashes (the ones SHADPS4_DEBUG_SKIP and
// SHADPS4_DEBUG_HIGHLIGHT take) and its colour blend to the pass log.
static bool LogShaders() {
    static const bool on = [] {
        const char* value = std::getenv("SHADPS4_LOG_PASSES_SHADERS");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return on;
}

static std::string ShaderText(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline) {
    static constexpr std::array<const char*, 5> names = {"ps", "hs", "ds", "vs", "gs"};
    std::string text = " | shaders";
    const auto& hashes = pipeline.GetGraphicsKey().stage_hashes;
    for (u32 stage = 0; stage < names.size(); ++stage) {
        if (hashes[stage] != 0) {
            text += fmt::format(" {} {:016x}", names[stage], hashes[stage]);
        }
    }
    const auto& blend = regs.blend_control[0];
    if (blend.enable) {
        text += fmt::format(", blend {} {} {}", magic_enum::enum_name(blend.color_src_factor),
                            magic_enum::enum_name(blend.color_func),
                            magic_enum::enum_name(blend.color_dst_factor));
    } else {
        text += ", no blend";
    }
    return text;
}

// One draw: `targets` holds the colour targets in slot order (empty ids for unused slots), then the
// depth target.
static void NoteDraw(const AmdGpu::Regs& regs, VideoCore::TextureCache& texture_cache,
                     std::span<const VideoCore::ImageId> bound_images,
                     std::span<const VideoCore::ImageId> colour, VideoCore::ImageId depth,
                     u64 indices, const GraphicsPipeline* pipeline) {
    std::string key = "draw into";
    std::vector<VideoCore::ImageId> own;
    for (u32 cb = 0; cb < colour.size(); ++cb) {
        if (const auto id = colour[cb]; id) {
            own.push_back(id);
            key += fmt::format(" C{} {}", cb, ImageText(texture_cache.GetImage(id)));
        }
    }
    if (depth) {
        own.push_back(depth);
        key += fmt::format(" D {}", ImageText(texture_cache.GetImage(depth)));
    }
    key += " | ";
    key += ViewportText(regs);
    if (regs.primitive_type != AmdGpu::PrimitiveType::TriangleList) {
        key += fmt::format(" | {}", magic_enum::enum_name(regs.primitive_type));
        if (regs.primitive_type == AmdGpu::PrimitiveType::PointList) {
            // PA_SU_POINT_SIZE (0xA280) and PA_SU_POINT_MINMAX (0xA281) hold half sizes in 12.4
            // fixed point: a full size in pixels is the value / 8.
            const u32 size = regs.reg_array[0xA280];
            const u32 minmax = regs.reg_array[0xA281];
            key += fmt::format(" {}x{} px (min {} max {}){}", (size >> 16) / 8.0,
                               (size & 0xffff) / 8.0, (minmax & 0xffff) / 8.0, (minmax >> 16) / 8.0,
                               regs.vs_output_control.use_vtx_point_size
                                   ? ", size from the vertex shader"
                                   : "");
            // Point sprites: SPI_PS_INPUT_CNTL_n (0xA191 + n) bit 17 PT_SPRITE_TEX has the
            // hardware put the sprite's texture coordinate into input n, laid out by
            // SPI_INTERP_CONTROL_0 (0xA1B5: PNT_SPRITE_ENA bit 1, overrides xyzw bits 2-13,
            // TOP_1 bit 14).
            u32 sprite_inputs = 0;
            for (u32 input = 0; input < 32; ++input) {
                if (regs.reg_array[0xA191 + input] & (1u << 17)) {
                    sprite_inputs |= 1u << input;
                }
            }
            key += fmt::format(", interp {:#x}, sprite-coord inputs {:#x}",
                               regs.reg_array[0xA1B5], sprite_inputs);
        }
    }
    if (pipeline != nullptr && LogShaders()) {
        key += ShaderText(regs, *pipeline);
    }
    const std::string reads = ReadTargetsText(texture_cache, bound_images, own, false);
    if (!reads.empty()) {
        key += " | reads ";
        key += reads;
    }
    Note(std::move(key), indices);
}

static void NoteDispatch(const AmdGpu::ComputeProgram& cs, VideoCore::TextureCache& texture_cache,
                         std::span<const VideoCore::ImageId> bound_images) {
    std::string key = fmt::format("dispatch {}x{}x{} groups of {}x{}x{}", cs.dim_x, cs.dim_y,
                                  cs.dim_z, cs.num_thread_x.full, cs.num_thread_y.full,
                                  cs.num_thread_z.full);
    const std::string images = ReadTargetsText(texture_cache, bound_images, {}, true);
    if (!images.empty()) {
        key += " | images ";
        key += images;
    }
    Note(std::move(key), 0);
}

// The targets a skipped draw would have used, read from the registers (the texture cache has not
// looked them up for it): pitch x height, data/number format codes, samples and guest address.
static std::string RegTargetsText(const AmdGpu::Regs& regs) {
    std::string text;
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            const auto& col_buf = regs.color_buffers[cb];
            if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
                continue;
            }
            text += fmt::format(" C{} {}x{} fmt {}/{} s{} @{:#x}", cb, col_buf.Pitch(),
                                col_buf.Height(), static_cast<u32>(col_buf.GetDataFmt()),
                                static_cast<u32>(col_buf.GetNumberFmt()), col_buf.NumSamples(),
                                col_buf.Address());
        }
    }
    const auto& db = regs.depth_buffer;
    if (db.DepthValid() || db.StencilValid()) {
        text += fmt::format(" D {}x{} zfmt {} s{} @{:#x}", db.Pitch(), db.Height(),
                            static_cast<u32>(db.z_info.format), db.NumSamples(),
                            db.DepthAddress());
    }
    return text.empty() ? std::string(" nothing") : text;
}

// Why FilterDraw turned a draw down (same order of checks).
static const char* FilterReason(const AmdGpu::Regs& regs) {
    using Mode = AmdGpu::ColorControl::OperationMode;
    if (regs.color_control.mode == Mode::EliminateFastClear) {
        return "fast-clear eliminate";
    }
    if (regs.color_control.mode == Mode::FmaskDecompress) {
        return "fmask decompress";
    }
    if (regs.color_control.mode == Mode::Resolve) {
        return "resolve";
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return "primitive type none";
    }
    return "depth/stencil copy";
}

// A draw the rasterizer did not issue as a draw: filtered out, or no pipeline for its stages.
static void NoteSkippedDraw(const AmdGpu::Regs& regs, const char* why, u64 indices) {
    Note(fmt::format("SKIPPED draw ({}, stages {:#x}) into{} | {}", why,
                     static_cast<u32>(regs.stage_enable.raw), RegTargetsText(regs),
                     ViewportText(regs)),
         indices);
}

// A dispatch the rasterizer did not issue as a dispatch (no pipeline, or done by the emulator).
static void NoteSkippedDispatch(const AmdGpu::ComputeProgram& cs, const char* why) {
    Note(fmt::format("SKIPPED dispatch ({}) {}x{}x{} groups of {}x{}x{}", why, cs.dim_x, cs.dim_y,
                     cs.dim_z, cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full),
         0);
}

// Shader hashes from an environment variable: hex, as the pass log prints them with
// SHADPS4_LOG_PASSES_SHADERS=1, comma separated.
static std::vector<u64> HashList(const char* name) {
    std::vector<u64> hashes;
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return hashes;
    }
    std::string_view text{value};
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string item{text.substr(0, comma)};
        if (!item.empty()) {
            hashes.push_back(std::strtoull(item.c_str(), nullptr, 16));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (!hashes.empty()) {
        LOG_INFO(Render_Vulkan, "{}: {} shader hash(es)", name, hashes.size());
    }
    return hashes;
}

static bool UsesShader(const GraphicsPipeline& pipeline, const std::vector<u64>& hashes) {
    for (const auto hash : pipeline.GetGraphicsKey().stage_hashes) {
        if (hash != 0 && std::find(hashes.begin(), hashes.end(), u64(hash)) != hashes.end()) {
            return true;
        }
    }
    return false;
}

// A draw into the right half of a side-by-side target (the right eye).
static bool RightEyeDraw(const AmdGpu::Regs& regs) {
    const auto& vp = regs.viewports[0];
    const auto& vp_ctl = regs.viewport_control;
    const float xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
    const float xscale = std::abs(vp_ctl.xscale_enable ? vp.xscale : 1.f);
    return !regs.IsClipDisabled() && xoffset - xscale > 0.5f;
}

// Debugging which draw shows what: SHADPS4_DEBUG_SKIP=<hash>[,...] drops the draws of pipelines
// using one of those shaders.
static bool DebugSkipped(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline) {
    static const std::vector<u64> skip = HashList("SHADPS4_DEBUG_SKIP");
    if (skip.empty() || !UsesShader(pipeline, skip)) {
        return false;
    }
    // SHADPS4_DEBUG_SKIP_RIGHT=1: only the draws into the right half of a side-by-side target (the
    // right eye), so the two eye captures of one frame show the picture with and without them.
    static const bool right_only = [] {
        const char* value = std::getenv("SHADPS4_DEBUG_SKIP_RIGHT");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return !right_only || RightEyeDraw(regs);
}

static std::string StageHashesText(const GraphicsPipeline& pipeline) {
    static constexpr std::array<const char*, 5> names = {"ps", "hs", "ds", "vs", "gs"};
    std::string text;
    const auto& hashes = pipeline.GetGraphicsKey().stage_hashes;
    for (u32 stage = 0; stage < names.size(); ++stage) {
        if (hashes[stage] != 0) {
            text += fmt::format("{}{} {:016x}", text.empty() ? "" : " ", names[stage], hashes[stage]);
        }
    }
    return text;
}

// SHADPS4_DEBUG_SWEEP=<step s>,<start s>[,right][,alt][,learn=<s>][,wait=<s>][,freeze][,until=<s>]:
// looks for draws that cover pixels but change nothing on screen (as the point sprites did when
// their texture coordinates were wrong). From <start> seconds after the first draw it notes for
// <learn> seconds (default the step, at least 3) which pipelines draw; then it drops them one at a
// time (only the right eye's draws with ",right", and then only pipelines that drew into the right
// eye), between a step with nothing dropped at each end (",alt": also between every two drops).
// A step with nothing dropped asks for one capture 70% into it. A step dropping a pipeline asks for
// up to two, each on a frame where that pipeline draws (from 30% into the step, 0.3 s apart), and
// lasts until it has both or <wait> seconds (default the step) have passed; a pipeline that did not
// draw in its step is logged as such. Every capture logs "SWEEP round <r> step <n>/<last>: ..."
// first, so the captures match the lines. Without ",right" the frame has to hold still and the
// captures are compared with the round's nothing-dropped ones; with it the two eyes are compared.
// ",freeze" (Super Stardust Ultra VR, during play): after learning, it watches 0.1 s windows of
// play; when one holds as many of the pipelines not yet swept as the best of the first 3 s (after
// 12 s: 80% of that), it freezes the game's world (WaitStats::SsuFreezeWorld), notes for 1 s which of those
// pipelines the still frame draws and sweeps them; then it lets the game run again and repeats for
// the rest (pipelines first drawn later join them) until all are swept or <until> seconds (from
// <start>) have passed. Without ",freeze", ",until=<s>" repeats rounds the same way (for ",right"
// during play, with vr.json "ipd_mm": 0 and a symmetric "fov_tan", which make both eyes the same).
// ",repeat=<n>": a pipeline counts as swept after n steps in which it drew (default 1), so with
// ",until" each one is dropped again in later rounds, at other moments of play (other enemies,
// explosions on screen).
static bool DebugSwept(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline) {
    struct Config {
        double step = 0.0;
        double start = 0.0;
        double learn = 0.0;
        double wait = 0.0;
        double until = 1e9;
        bool right = false;
        bool alt = false;
        bool freeze = false;
        u32 repeat = 1;
    };
    static const Config config = [] {
        Config c;
        const char* value = std::getenv("SHADPS4_DEBUG_SWEEP");
        if (value == nullptr) {
            return c;
        }
        c.step = std::atof(value);
        if (const char* comma = std::strchr(value, ','); comma != nullptr) {
            const char* rest = comma + 1;
            c.start = std::atof(rest);
            c.right = std::strstr(rest, "right") != nullptr;
            c.alt = std::strstr(rest, "alt") != nullptr;
            c.freeze = std::strstr(rest, "freeze") != nullptr;
            if (const char* learn = std::strstr(rest, "learn="); learn != nullptr) {
                c.learn = std::atof(learn + 6);
            }
            if (const char* wait = std::strstr(rest, "wait="); wait != nullptr) {
                c.wait = std::atof(wait + 5);
            }
            if (const char* repeat = std::strstr(rest, "repeat="); repeat != nullptr) {
                c.repeat = static_cast<u32>(std::max(1, std::atoi(repeat + 7)));
            }
            if (const char* until = std::strstr(rest, "until="); until != nullptr) {
                c.until = std::atof(until + 6);
            }
        }
        c.learn = c.learn > 0.0 ? c.learn : std::max(c.step, 3.0);
        c.wait = std::max(c.wait, c.step);
        return c;
    }();
    if (config.step <= 0.0) {
        return false;
    }
    using Clock = std::chrono::steady_clock;
    static const auto first_draw = Clock::now();
    static std::mutex sweep_mutex;
    struct Seen {
        const GraphicsPipeline* pipeline;
        u64 draws;
        u64 right_draws;
    };
    enum class Phase { Learn, Hunt, Settle, StillLearn, Sweep, Done };
    static Phase phase = Phase::Learn;
    static std::vector<Seen> seen;                          // every pipeline drawn during play
    static std::vector<const GraphicsPipeline*> done_list;  // swept (or not sweepable) already
    static std::vector<std::pair<const GraphicsPipeline*, u32>> hits; // steps in which each drew
    static std::vector<Seen> frame; // the still frame's pipelines / the window being watched
    static std::vector<Seen> swept; // this round's, in order
    static double phase_start = 0.0;
    static double window_start = 0.0;
    static size_t peak = 0;
    static u32 round = 0;
    static s64 step = 0;
    static s64 last = 0;
    static double step_start = 0.0;
    static double last_capture = 0.0;
    static u32 captures = 0;
    static u64 step_draws = 0;

    const double t =
        std::chrono::duration<double>(Clock::now() - first_draw).count() - config.start;
    if (t < 0.0) {
        return false;
    }
    std::scoped_lock lock{sweep_mutex};
    if (phase == Phase::Done) {
        return false;
    }
    const auto note = [&](std::vector<Seen>& list) {
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const Seen& s) { return s.pipeline == &pipeline; });
        if (it == list.end()) {
            list.push_back({&pipeline, 0, 0});
            it = std::prev(list.end());
        }
        it->draws++;
        it->right_draws += RightEyeDraw(regs) ? 1 : 0;
    };
    const auto is_done = [&](const GraphicsPipeline* p) {
        return std::find(done_list.begin(), done_list.end(), p) != done_list.end();
    };
    const auto sweepable = [&](const Seen& s) {
        return !is_done(s.pipeline) && (!config.right || s.right_draws != 0);
    };
    const auto pending = [&] {
        size_t n = 0;
        for (const auto& s : seen) {
            n += sweepable(s) ? 1 : 0;
        }
        return n;
    };
    const auto freeze = [&](bool on) {
        if (!Libraries::Kernel::WaitStats::SsuFreezeWorld(on) && on) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARNING(Render_Vulkan, "SWEEP cannot find the game's frame timer");
            }
        }
    };
    const auto finish = [&](const char* why) {
        freeze(false);
        std::string left;
        for (const auto& s : seen) {
            if (!is_done(s.pipeline)) {
                left += fmt::format("\n    {} ({} draws, {} right eye)",
                                    StageHashesText(*s.pipeline), s.draws, s.right_draws);
            }
        }
        LOG_INFO(Render_Vulkan, "SWEEP done ({}): {} of {} pipelines swept{}{}", why,
                 done_list.size(), seen.size(), left.empty() ? "" : "; not swept:", left);
        phase = Phase::Done;
    };
    const auto hunt = [&] {
        phase = Phase::Hunt;
        phase_start = t;
        window_start = t;
        peak = 0;
        frame.clear();
    };
    const auto start_round = [&](const std::vector<Seen>& candidates) {
        swept.clear();
        for (const auto& s : candidates) {
            if (is_done(s.pipeline)) {
                continue;
            }
            if (config.right && s.right_draws == 0) {
                LOG_INFO(Render_Vulkan, "SWEEP skips {} ({} draws, none into the right eye)",
                         StageHashesText(*s.pipeline), s.draws);
                done_list.push_back(s.pipeline);
                continue;
            }
            swept.push_back(s);
        }
        ++round;
        last = config.alt ? 2 * s64(swept.size()) + 1 : s64(swept.size()) + 2;
        LOG_INFO(Render_Vulkan, "SWEEP round {}: drops {} pipelines, {} steps", round,
                 swept.size(), last);
        step = 1;
        step_start = t;
        captures = 0;
        step_draws = 0;
        phase = Phase::Sweep;
    };

    switch (phase) {
    case Phase::Learn:
        if (t < config.learn) {
            note(seen);
            return false;
        }
        LOG_INFO(Render_Vulkan, "SWEEP learned {} pipelines in {:g} s", seen.size(),
                 config.learn);
        if (!config.freeze) {
            start_round(seen);
            break;
        }
        hunt();
        [[fallthrough]];
    case Phase::Hunt: {
        if (t >= config.until) {
            finish("time is up");
            return false;
        }
        note(seen); // pipelines first drawn now join
        if (t - window_start >= 0.1) {
            size_t count = 0;
            for (const auto& s : frame) {
                count += sweepable(s) ? 1 : 0;
            }
            const double hunting = t - phase_start;
            bool go = false;
            if (hunting < 3.0) {
                peak = std::max(peak, count);
            } else {
                go = count > 0 && (count >= peak || (hunting >= 12.0 && count * 5 >= peak * 4));
            }
            frame.clear();
            window_start = t;
            if (go) {
                LOG_INFO(Render_Vulkan,
                         "SWEEP: {} of the {} pipelines not yet swept drew in the last 0.1 s",
                         count, pending());
                freeze(true);
                phase = Phase::Settle;
                phase_start = t;
                return false;
            }
        }
        note(frame);
        return false;
    }
    case Phase::Settle:
        freeze(true);
        if (t - phase_start >= 0.5) {
            phase = Phase::StillLearn;
            phase_start = t;
            frame.clear();
        }
        return false;
    case Phase::StillLearn:
        freeze(true);
        note(frame);
        if (t - phase_start < 1.0) {
            return false;
        }
        start_round(frame);
        if (swept.empty()) {
            freeze(false);
            hunt();
            return false;
        }
        break;
    case Phase::Sweep:
    case Phase::Done:
        break;
    }
    if (config.freeze) {
        freeze(true);
    }

    const auto target_of = [&](s64 n) -> s64 {
        if (config.alt) {
            return n % 2 == 0 ? n / 2 - 1 : -1;
        }
        return n == 1 || n == last ? -1 : n - 2;
    };
    note(seen); // pipelines first drawn now join the next round
    // Move on when the step is over.
    for (;;) {
        const s64 index = target_of(step);
        const double into = t - step_start;
        const bool over = index < 0 ? captures >= 1 && into >= config.step
                                    : (captures >= 2 && into >= config.step) || into >= config.wait;
        if (!over) {
            break;
        }
        if (index >= 0) {
            if (step_draws == 0) {
                LOG_INFO(Render_Vulkan, "SWEEP round {} step {}/{}: {} did not draw (no capture)",
                         round, step, last, StageHashesText(*swept[index].pipeline));
            } else {
                const auto* p = swept[index].pipeline;
                auto hit = std::find_if(hits.begin(), hits.end(),
                                        [&](const auto& h) { return h.first == p; });
                if (hit == hits.end()) {
                    hits.emplace_back(p, 0);
                    hit = std::prev(hits.end());
                }
                if (++hit->second >= config.repeat) {
                    done_list.push_back(p);
                }
            }
        }
        ++step;
        step_start = t;
        captures = 0;
        step_draws = 0;
        if (step > last) {
            if (!config.freeze) {
                // Without ",until" one round; with it, rounds for the pipelines that did not
                // draw in their step or were first drawn later, until the time is up.
                LOG_INFO(Render_Vulkan, "SWEEP round {} done; {} pipelines left", round,
                         pending());
                if (config.until >= 1e8 || pending() == 0 || t >= config.until) {
                    finish(pending() == 0 ? "all swept" : "time is up");
                    return false;
                }
                start_round(seen);
                return false;
            }
            freeze(false);
            LOG_INFO(Render_Vulkan, "SWEEP round {} done; {} pipelines left", round, pending());
            if (pending() == 0) {
                finish("all swept");
                return false;
            }
            hunt();
            return false;
        }
    }
    const s64 index = target_of(step);
    const auto* target = index < 0 ? nullptr : swept[index].pipeline;
    const bool dropped = target == &pipeline && (!config.right || RightEyeDraw(regs));
    const double into = t - step_start;
    bool capture = false;
    if (target == nullptr) {
        capture = captures == 0 && into >= 0.7 * config.step;
    } else if (dropped) {
        ++step_draws;
        capture = captures < 2 && into >= 0.3 * config.step &&
                  (captures == 0 || t - last_capture >= 0.3);
    }
    if (capture) {
        ++captures;
        last_capture = t;
        if (target == nullptr) {
            LOG_INFO(Render_Vulkan, "SWEEP round {} step {}/{}: nothing dropped", round, step,
                     last);
        } else {
            const auto& s = swept[index];
            LOG_INFO(Render_Vulkan,
                     "SWEEP round {} step {}/{}: dropped {} ({}) | capture {}, {} draws "
                     "({} right eye)",
                     round, step, last, StageHashesText(*target),
                     config.right ? "right eye" : "all", captures, s.draws, s.right_draws);
        }
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
    }
    return dropped;
}

// SHADPS4_DEBUG_HIGHLIGHT=<hash>[:<channels>][,...]: those pipelines set the channels of every pixel
// they draw (logic op SET, see GraphicsPipeline::Compile); here the colour write mask that picks the
// channels: letters r, g, b (default rb, magenta), so several pipelines can be told apart in a run.
static std::optional<vk::ColorComponentFlags> DebugHighlight(const GraphicsPipeline& pipeline) {
    static const std::vector<std::pair<u64, vk::ColorComponentFlags>> highlight = [] {
        std::vector<std::pair<u64, vk::ColorComponentFlags>> list;
        const char* value = std::getenv("SHADPS4_DEBUG_HIGHLIGHT");
        std::string_view text{value != nullptr ? value : ""};
        while (!text.empty()) {
            const size_t comma = text.find(',');
            const std::string item{text.substr(0, comma)};
            const size_t colon = item.find(':');
            vk::ColorComponentFlags mask{};
            if (colon != std::string::npos) {
                for (const char c : item.substr(colon + 1)) {
                    mask |= c == 'r'   ? vk::ColorComponentFlagBits::eR
                            : c == 'g' ? vk::ColorComponentFlagBits::eG
                            : c == 'b' ? vk::ColorComponentFlagBits::eB
                                       : vk::ColorComponentFlags{};
                }
            }
            if (!mask) {
                mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eB;
            }
            if (!item.empty()) {
                list.emplace_back(std::strtoull(item.substr(0, colon).c_str(), nullptr, 16), mask);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            text.remove_prefix(comma + 1);
        }
        return list;
    }();
    for (const auto hash : pipeline.GetGraphicsKey().stage_hashes) {
        for (const auto& [wanted, mask] : highlight) {
            if (hash != 0 && u64(hash) == wanted) {
                return mask;
            }
        }
    }
    return std::nullopt;
}

// Guest memory as dwords (the CPU's copy: data a GPU pass wrote may not be there yet).
static std::string GuestDwords(Core::MemoryManager* memory, VAddr address, u32 count,
                               bool as_float) {
    if (address == 0 || memory->ClampRangeSize(address, u64(count) * 4) < u64(count) * 4) {
        return " (unmapped)";
    }
    std::vector<u32> data(count);
    std::memcpy(data.data(), reinterpret_cast<const void*>(address), data.size() * 4);
    std::string text;
    for (const u32 value : data) {
        if (as_float) {
            float f;
            std::memcpy(&f, &value, 4);
            text += fmt::format(" {:g}", f);
        } else {
            text += fmt::format(" {:08x}", value);
        }
    }
    return text;
}

// SHADPS4_DEBUG_DESCRIBE=<hash>[,...]: logs the first three draws of pipelines using one of those
// shaders in detail - pixel shader input mapping, vertex attributes with the first vertices' data,
// constant buffers and textures - to find which input of a draw that comes out black is zero.
static void DebugDescribe(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                          Core::MemoryManager* memory, VideoCore::TextureCache& texture_cache,
                          std::span<const VideoCore::ImageId> bound, u32 index_offset,
                          bool is_indexed) {
    static const std::vector<u64> hashes = HashList("SHADPS4_DEBUG_DESCRIBE");
    static u32 described = 0;
    if (hashes.empty() || described >= 3 || !UsesShader(pipeline, hashes)) {
        return;
    }
    // SHADPS4_DEBUG_DESCRIBE_FROM=<s>: only draws from so many seconds after the first one.
    static const double from = [] {
        const char* value = std::getenv("SHADPS4_DEBUG_DESCRIBE_FROM");
        return value != nullptr ? std::atof(value) : 0.0;
    }();
    static const auto first = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() < from) {
        return;
    }
    ++described;
    LOG_INFO(Render_Vulkan, "DESCRIBE draw {}: {} indices, {} instances, indexed {}, prim {}",
             described, regs.num_indices, regs.num_instances.NumInstances(), is_indexed,
             magic_enum::enum_name(regs.primitive_type));
    const auto& vs = pipeline.GetStage(Shader::SwStage::Vertex);
    const auto& ps = pipeline.GetStage(Shader::SwStage::Fragment);
    LOG_INFO(Render_Vulkan, "  vs exports {}, ps num_interp {}", regs.vs_output_config.NumExports(),
             u32(regs.num_interp));
    for (u32 i = 0; i < regs.num_interp; ++i) {
        const auto& input = regs.ps_inputs[i];
        LOG_INFO(Render_Vulkan,
                 "  ps input {}: offset {} use_default {} default {} flat {}, interp {} {}, "
                 "reads {} comps",
                 i, u32(input.input_offset), u32(input.use_default), u32(input.default_value),
                 u32(input.flat_shade), magic_enum::enum_name(ps.fs_interpolation[i].primary),
                 magic_enum::enum_name(ps.fs_interpolation[i].auxiliary),
                 ps.loads.NumComponents(Shader::IR::Attribute::Param0 + i));
    }
    for (u32 i = 0; i < Shader::IR::NumParams; ++i) {
        if (vs.stores.GetAny(Shader::IR::Attribute::Param0 + i)) {
            LOG_INFO(Render_Vulkan, "  vs stores param {} ({} comps)", i,
                     vs.stores.NumComponents(Shader::IR::Attribute::Param0 + i));
        }
    }
    // The first indices of the draw, then those vertices' attributes.
    std::vector<u32> indices;
    const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    if (is_indexed) {
        const u32 size = index16 ? 2 : 4;
        const VAddr base = regs.index_base_address.Address<VAddr>() + u64(index_offset) * size;
        const u32 count = std::min(regs.num_indices, 6u);
        if (memory->ClampRangeSize(base, u64(count) * size) >= u64(count) * size) {
            for (u32 i = 0; i < count; ++i) {
                u32 value = 0;
                std::memcpy(&value, reinterpret_cast<const void*>(base + u64(i) * size), size);
                indices.push_back(value);
            }
        }
    } else {
        for (u32 i = 0; i < std::min(regs.num_indices, 6u); ++i) {
            indices.push_back(i);
        }
    }
    std::string index_text;
    for (const u32 index : indices) {
        index_text += fmt::format(" {}", index);
    }
    const auto& fetch = pipeline.GetFetchShader();
    u32 vertex_offset = regs.index_offset;
    if (!fetch.Empty() && vertex_offset == 0 && fetch.vertex_offset_sgpr != -1) {
        vertex_offset = vs.user_data[fetch.vertex_offset_sgpr];
    }
    LOG_INFO(Render_Vulkan, "  first indices{} (index16 {}), vertex offset {}", index_text,
             index16, vertex_offset);
    for (u32 a = 0; a < fetch.attributes.size(); ++a) {
        const auto& attrib = fetch.attributes[a];
        const auto vsharp = attrib.GetSharp(vs);
        LOG_INFO(Render_Vulkan,
                 "  attribute {}: vgpr {} elements {} step {}, buffer {:#x} stride {} records {} "
                 "format {} {} swizzle {}{}{}{}",
                 a, u32(attrib.dest_vgpr), u32(attrib.num_elements), u32(attrib.instance_data),
                 u64(vsharp.base_address), u32(vsharp.stride), vsharp.num_records,
                 magic_enum::enum_name(vsharp.GetDataFmt()),
                 magic_enum::enum_name(vsharp.GetNumberFmt()), u32(vsharp.dst_sel_x),
                 u32(vsharp.dst_sel_y), u32(vsharp.dst_sel_z), u32(vsharp.dst_sel_w));
        const u32 stride = std::max<u32>(u32(vsharp.stride), 4u);
        for (u32 v = 0; v < std::min<size_t>(indices.size(), 4); ++v) {
            const VAddr address =
                vsharp.base_address + u64(indices[v] + vertex_offset) * stride;
            LOG_INFO(Render_Vulkan, "    vertex {}:{}", indices[v] + vertex_offset,
                     GuestDwords(memory, address, std::min<u32>(stride / 4, 8u), false));
        }
    }
    const auto describe_buffers = [&](const char* stage, const Shader::Info& info) {
        for (u32 b = 0; b < info.buffers.size(); ++b) {
            const auto vsharp = info.buffers[b].GetSharp(info);
            LOG_INFO(Render_Vulkan, "  {} buffer {}: {:#x} records {} stride {}:{}", stage, b,
                     u64(vsharp.base_address), vsharp.num_records, u32(vsharp.stride),
                     GuestDwords(memory, vsharp.base_address,
                                 128u, true));
        }
    };
    describe_buffers("vs", vs);
    describe_buffers("ps", ps);
    const auto describe_flat = [&](const char* stage, const Shader::Info& info) {
        std::string text;
        for (u32 i = 0; i < std::min<size_t>(info.flattened_ud_buf.size(), 40); ++i) {
            float f;
            std::memcpy(&f, &info.flattened_ud_buf[i], 4);
            text += fmt::format(" {:g}", f);
        }
        LOG_INFO(Render_Vulkan, "  {} flattened user data ({} dwords):{}", stage,
                 info.flattened_ud_buf.size(), text);
    };
    describe_flat("vs", vs);
    describe_flat("ps", ps);
    // SHADPS4_DEBUG_DESCRIBE_DUMP=<folder>: also write the first 2 MB of each texture's guest
    // memory there (first draw only), to decode the picture offline.
    static const char* dump_dir = std::getenv("SHADPS4_DEBUG_DESCRIBE_DUMP");
    for (u32 t = 0; t < ps.images.size(); ++t) {
        const auto tsharp = ps.images[t].GetSharp(ps);
        LOG_INFO(Render_Vulkan,
                 "  ps texture {}: {:#x} {}x{} {} {} tiling {} type {} swizzle {}{}{}{} "
                 "levels {}-{}, pitch {}, first bytes{}",
                 t, tsharp.Address(), u32(tsharp.width) + 1, u32(tsharp.height) + 1,
                 magic_enum::enum_name(AmdGpu::DataFormat(tsharp.data_format)),
                 magic_enum::enum_name(AmdGpu::NumberFormat(tsharp.num_format)),
                 u32(tsharp.tiling_index), u32(tsharp.type), u32(tsharp.dst_sel_x),
                 u32(tsharp.dst_sel_y), u32(tsharp.dst_sel_z), u32(tsharp.dst_sel_w),
                 u32(tsharp.base_level), u32(tsharp.last_level), u32(tsharp.pitch) + 1,
                 GuestDwords(memory, tsharp.Address(), 8, false));
        constexpr u64 DumpSize = 2_MB;
        if (dump_dir != nullptr && *dump_dir != '\0' && described == 1 &&
            memory->ClampRangeSize(tsharp.Address(), DumpSize) >= DumpSize) {
            const auto path = fmt::format("{}/describe_tex{}_{:x}.bin", dump_dir, t,
                                          tsharp.Address());
            if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                std::fwrite(reinterpret_cast<const void*>(tsharp.Address()), 1, DumpSize, file);
                std::fclose(file);
                LOG_INFO(Render_Vulkan, "  wrote {}", path);
            }
        }
    }
    for (const auto id : bound) {
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(id);
        LOG_INFO(Render_Vulkan, "  bound image {}{}{}", ImageText(image),
                 image.info.props.is_block ? " (block)" : "",
                 image.usage.render_target ? " (render target)" : "");
    }
}

} // namespace PassLog

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() = default;

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.image_id = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(liverpool->regs, PassLog::FilterReason(liverpool->regs),
                                     u64(liverpool->regs.num_indices));
        }
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(regs, "no pipeline", u64(regs.num_indices));
        }
        return;
    }
    if (PassLog::DebugSkipped(liverpool->regs, *pipeline) ||
        PassLog::DebugSwept(liverpool->regs, *pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(regs, "SHADPS4_DEBUG_SKIP", u64(regs.num_indices));
        }
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(regs, "resources not bound", u64(regs.num_indices));
        }
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (PassLog::Interval() != 0) {
        std::vector<VideoCore::ImageId> colour;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            colour.push_back(cb_descs[cb].image_id);
        }
        PassLog::NoteDraw(regs, texture_cache, bound_images, colour, db_desc.image_id,
                          u64(regs.num_indices) * regs.num_instances.NumInstances(), pipeline);
    }
    PassLog::DebugDescribe(regs, *pipeline, memory, texture_cache, bound_images, index_offset,
                           is_indexed);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    pipeline->BindResources(set_writes, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(liverpool->regs, PassLog::FilterReason(liverpool->regs), 0);
        }
        return;
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(liverpool->regs, "no pipeline (indirect)", 0);
        }
        return;
    }
    if (PassLog::DebugSkipped(liverpool->regs, *pipeline) ||
        PassLog::DebugSwept(liverpool->regs, *pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(liverpool->regs, "SHADPS4_DEBUG_SKIP (indirect)", 0);
        }
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDraw(liverpool->regs, "resources not bound (indirect)", 0);
        }
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (PassLog::Interval() != 0) {
        std::vector<VideoCore::ImageId> colour;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            colour.push_back(cb_descs[cb].image_id);
        }
        PassLog::NoteDraw(liverpool->regs, texture_cache, bound_images, colour, db_desc.image_id,
                          0, pipeline);
    }

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(0, true);
    }

    const auto size = stride * max_count;
    const auto [buffer, base] = buffer_cache.ObtainBuffer(arg_address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    bound_buffers.emplace_back(buffer, base, size, vk::AccessFlagBits2::eIndirectCommandRead);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
        bound_buffers.emplace_back(count_buffer, count_offset, 4,
                                   vk::AccessFlagBits2::eIndirectCommandRead);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    pipeline->BindResources(set_writes, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    ResetBindings(false);
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDispatch(cs_program, "no pipeline");
        }
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDispatch(cs_program, "shader HLE");
        }
        return;
    }

    if (!BindResources(pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDispatch(cs_program, "image copy/clear done by the emulator");
        }
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    if (PassLog::Interval() != 0) {
        PassLog::NoteDispatch(cs_program, texture_cache, bound_images);
    }

    ResetBindings(true);
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDispatch(cs_program, "no pipeline (indirect)");
        }
        return;
    }

    if (!BindResources(pipeline)) {
        if (PassLog::Interval() != 0) {
            PassLog::NoteSkippedDispatch(cs_program, "image copy/clear done by the emulator (indirect)");
        }
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    bound_buffers.emplace_back(buffer, base, size, vk::AccessFlagBits2::eIndirectCommandRead);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    if (PassLog::Interval() != 0) {
        PassLog::NoteDispatch(cs_program, texture_cache, bound_images);
    }

    ResetBindings(true);
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

void Rasterizer::OnFence() {
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    const auto& regs = liverpool->regs;
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        bound_buffers.emplace_back(range.buffer, range.offset, size,
                                   vk::AccessFlagBits2::eVertexAttributeRead);
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset, bool is_indirect) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;

    // Bind index buffer.
    const u32 num_indices =
        is_indirect ? regs.max_index_size : std::min(regs.num_indices, regs.max_index_size);
    const u64 requested_size = u64(num_indices) * index_size;
    const u64 mapped_size = memory->ClampRangeSize(index_address, requested_size);
    u32 index_buffer_size = static_cast<u32>(Common::AlignDown(mapped_size, index_size));
    vk::Buffer index_buffer{};
    u64 buffer_offset{};
    if (index_buffer_size != 0) {
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
        needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
        bound_buffers.emplace_back(buffer, offset, index_buffer_size,
                                   vk::AccessFlagBits2::eIndexRead);
        index_buffer = buffer->Handle();
        buffer_offset = offset;
    } else if (!instance.IsMaintenance6Supported()) {
        const auto* null_buffer = buffer_cache.GetNullIndexBuffer();
        index_buffer_size = static_cast<u32>(null_buffer->SizeBytes());
        needs_barrier |= runtime.IsBufferAccessed(null_buffer, 0, index_buffer_size);
        bound_buffers.emplace_back(null_buffer, 0, index_buffer_size,
                                   vk::AccessFlagBits2::eIndexRead);
        index_buffer = null_buffer->Handle();
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    if (instance.IsMaintenance5Supported()) {
        cmdbuf.bindIndexBuffer2KHR(index_buffer, buffer_offset, index_buffer_size, index_type);
    } else {
        cmdbuf.bindIndexBuffer(index_buffer, buffer_offset, index_type);
    }
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, src_access] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        runtime.AccessBuffer(buffer, offset, size, dst_stage, src_access);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
    static const u32 submit_batch_size = [] {
        const char* value = std::getenv("SHADPS4_SUBMIT_BATCH_SIZE");
        const long parsed = value ? std::strtol(value, nullptr, 10) : 0;
        const u32 size = parsed > 0 ? static_cast<u32>(std::clamp(parsed, 32l, 4096l)) : 0;
        LOG_INFO(Render_Vulkan, "Rendering submission batch size: {} (0 disables)", size);
        return size;
    }();
    if (submit_batch_size != 0) {
        const u64 tick = scheduler.CurrentTick();
        if (tick != draw_batch_tick) {
            draw_batch_tick = tick;
            draw_batch_commands = 0;
        }
        if (++draw_batch_commands >= submit_batch_size) {
            scheduler.Flush();
        }
    }
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    const u64 alignment = instance.StorageMinAlignment();
    for (const auto& desc : stage.buffers) {
        const auto src_access =
            vk::AccessFlagBits2::eShaderRead |
            (desc.is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone);
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), src_access);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->regs.clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, src_access);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

// At a raised render scale the game's bloom pass reads the whole scene (7680x4320 at 400%) into its
// fixed 480x540 buffer with the few samples per pixel made for its own scene size (1920x1080), so
// most of the scene is skipped and small bright things (shots, sparks) flicker in and out of the
// glow. A read like that, a screen-sized picture into a target many times smaller, gets a copy
// filtered down to the size the game reads at its own scale instead: the larger side ratio to the
// target brought back to the ratio below (400%: 7680x4320 -> 3840x2160 -> 1920x1080 for the 480x540
// bloom buffer, the 4x2 the game made its samples for). Halves where it can (a linear blit to half
// the size is an exact 2x2 box), one last linear blit to that size where it cannot (300%).
// SHADPS4_DOWNSAMPLE_READS=0 turns this off; =<n> sets the size ratio above which a read is filtered
// (default 4, which the game's own sizes stay within).
namespace ReadDownsample {

static u32 Ratio() {
    static const u32 ratio = [] {
        const char* value = std::getenv("SHADPS4_DOWNSAMPLE_READS");
        u32 result = 4;
        if (value != nullptr && *value != '\0') {
            const int n = std::atoi(value);
            result = n <= 0 ? 0u : u32(std::max(n, 2));
        }
        if (result == 0) {
            LOG_INFO(Render_Vulkan, "Downsampled reads: off (SHADPS4_DOWNSAMPLE_READS=0)");
        } else {
            LOG_INFO(Render_Vulkan, "Downsampled reads: pictures over {}x the target size", result);
        }
        return result;
    }();
    return ratio;
}

struct Level {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 width{};
    u32 height{};
};

struct Chain {
    vk::Format format{};
    AmdGpu::CompMapping mapping{};
    u32 src_width{};
    u32 src_height{};
    u32 dst_width{};
    u32 dst_height{};
    std::vector<std::unique_ptr<Level>> levels;
};

// Never freed: freeing at exit would run after the device is gone.
static std::vector<std::unique_ptr<Chain>>& Chains() {
    static auto* chains = new std::vector<std::unique_ptr<Chain>>();
    return *chains;
}

static Chain& GetChain(const Instance& instance, vk::Format format,
                       const AmdGpu::CompMapping& mapping, u32 src_width, u32 src_height,
                       u32 dst_width, u32 dst_height) {
    for (auto& chain : Chains()) {
        if (chain->format == format && chain->mapping == mapping &&
            chain->src_width == src_width && chain->src_height == src_height &&
            chain->dst_width == dst_width && chain->dst_height == dst_height) {
            return *chain;
        }
    }
    auto chain = std::make_unique<Chain>();
    chain->format = format;
    chain->mapping = mapping;
    chain->src_width = src_width;
    chain->src_height = src_height;
    chain->dst_width = dst_width;
    chain->dst_height = dst_height;
    const auto components = mapping == AmdGpu::IdentityMapping
                                 ? vk::ComponentMapping{}
                                 : LiverpoolToVK::ComponentMapping(mapping);
    const double over =
        std::max(double(src_width) / dst_width, double(src_height) / dst_height) / Ratio();
    const u32 final_width = std::max(1u, u32(std::lround(src_width / over)));
    const u32 final_height = std::max(1u, u32(std::lround(src_height / over)));
    u32 width = src_width;
    u32 height = src_height;
    std::string sizes;
    while (width != final_width || height != final_height) {
        if (width >= 2 * final_width && height >= 2 * final_height) {
            width = (width + 1) / 2;
            height = (height + 1) / 2;
        } else {
            width = final_width;
            height = final_height;
        }
        auto level = std::make_unique<Level>();
        level->width = width;
        level->height = height;
        level->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        level->image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto [result, view] = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = level->image.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = components,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(result));
        level->view = std::move(view);
        sizes += fmt::format(" -> {}x{}", width, height);
        chain->levels.push_back(std::move(level));
    }
    LOG_INFO(Render_Vulkan, "Downsampled reads: {} {}x{}{} for a {}x{} target",
             vk::to_string(format), src_width, src_height, sizes, dst_width, dst_height);
    return *Chains().emplace_back(std::move(chain));
}

// The view to bind instead of `image` for a fragment-shader texture read into a target of the given
// size, filled now; or a null handle when the read is left as it is.
static vk::ImageView Substitute(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                                VideoCore::Image& image, const VideoCore::ImageViewInfo& view_info,
                                u32 target_width, u32 target_height) {
    const u32 ratio = Ratio();
    if (ratio == 0 || target_width == 0 || target_height == 0) {
        return {};
    }
    const auto& range = view_info.range;
    if (image.info.props.is_depth || image.info.num_samples > 1 ||
        image.info.resources.levels != 1 || range.base.level != 0 || range.extent.layers != 1 ||
        view_info.type != AmdGpu::ImageType::Color2D || view_info.format != image.info.pixel_format) {
        return {};
    }
    const u32 src_width = image.info.size.width;
    const u32 src_height = image.info.size.height;
    if (u64(src_width) * src_height < 1920ull * 1080ull ||
        (src_width <= ratio * target_width && src_height <= ratio * target_height)) {
        return {};
    }
    auto& chain = GetChain(instance, image.info.pixel_format, view_info.mapping, src_width,
                           src_height, target_width, target_height);
    if (chain.levels.empty()) {
        return {};
    }

    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferRead, range);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    vk::Image src = image.GetImage();
    u32 width = src_width;
    u32 height = src_height;
    vk::ImageSubresourceLayers src_layers{vk::ImageAspectFlagBits::eColor, 0, range.base.layer, 1};
    for (size_t i = 0; i < chain.levels.size(); ++i) {
        auto& level = *chain.levels[i];
        const bool last = i + 1 == chain.levels.size();
        // Wait for the reads of the last frame's copy, then overwrite it.
        const vk::ImageMemoryBarrier2 before = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = level.image.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &before,
        });
        // A linear blit to half the size averages each 2x2 block exactly.
        const vk::ImageBlit blit = {
            .srcSubresource = src_layers,
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{s32(width), s32(height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{s32(level.width), s32(level.height), 1}},
        };
        cmdbuf.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, level.image.image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        const vk::ImageMemoryBarrier2 after = {
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = last ? vk::PipelineStageFlagBits2::eAllCommands
                                 : vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask =
                last ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = last ? vk::ImageLayout::eShaderReadOnlyOptimal
                              : vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = level.image.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &after,
        });
        src = level.image.image;
        width = level.width;
        height = level.height;
        src_layers = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    }
    return *chain.levels.back()->view;
}

} // namespace ReadDownsample

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    // The colour target's size, for ReadDownsample (fragment-shader reads only).
    u32 target_width = 0;
    u32 target_height = 0;
    if (stage.sw_stage == Shader::SwStage::Fragment) {
        const auto& regs = liverpool->regs;
        if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable &&
            regs.color_buffers[0] && regs.color_target_mask.GetMask(0) && cb_descs[0].image_id) {
            const auto& target = texture_cache.GetImage(cb_descs[0].image_id);
            target_width = target.info.size.width;
            target_height = target.info.size.height;
        }
    }

    u32 num_images{};
    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings[num_images++].image_id = {};
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings[num_images++].image_id = {};
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings[num_images++];
            std::construct_at(&desc, tsharp, image_desc);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 i = 0; i < num_images; ++i) {
        auto& [image_id, desc] = image_bindings[i];
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            const auto binding = image.binding;

            if (!is_storage && target_width != 0 && !binding.is_target && !binding.force_general) {
                if (const vk::ImageView small =
                        ReadDownsample::Substitute(instance, scheduler, runtime, image,
                                                   desc.view_info, target_width, target_height)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, small,
                                             vk::ImageLayout::eShaderReadOnlyOptimal);
                    continue;
                }
            }

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    needs_barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->regs.ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        liverpool->regs.ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base, sampler.is_depth);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.image_id; image_id) {
        auto& desc = db_desc.desc;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    const auto write_id = texture_cache.FindImage(write_desc);
    auto& write_image = texture_cache.GetImage(write_id);
    // The copy destination is never bound as a target, so its stencil plane is linked here.
    texture_cache.AssociateStencil(write_id, write_desc.info);

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    if (const auto highlight = PassLog::DebugHighlight(*pipeline)) {
        auto masks = pipeline->GetGraphicsKey().write_masks;
        for (auto& mask : masks) {
            mask = *highlight;
        }
        dynamic_state.SetColorWriteMasks(masks);
    } else {
        dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    }
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

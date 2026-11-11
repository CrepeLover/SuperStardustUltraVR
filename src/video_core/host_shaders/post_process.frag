// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler[2];

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    // Above 0, the picture is sharpened by so much (up to 1): for one that is shown larger
    // than it was drawn, which blurs it.
    float sharpen;
    // The target encodes what it is given for display by itself (an sRGB image): it has to be
    // given linear light, or the picture would be encoded twice.
    bool linear_out;
    vec4 source_uv;
    vec4 overlay_uv;
    bool has_overlay;
    bool srgb_input;
    float gamma_cutoff;
    float gamma_slope;
    float gamma_exponent;
    float dither_quantum;
    // Above 0, edges in the eye's own picture (not the overlay drawn over it) are smoothed
    // first by morphological anti-aliasing (MLAA), at that strength (up to 1); a pixel smoothed
    // so is then not sharpened.
    float mlaa;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(max(rgb, vec3(0.0)), vec3(pp.gamma_exponent)) - b,
        pp.gamma_slope * max(rgb, vec3(0.0)),
        lessThan(rgb, vec3(pp.gamma_cutoff))
    );
}

// A place in the eye's part of the source, kept half a pixel inside it.
vec2 inside(vec2 at) {
    vec2 half_texel = 0.5 / vec2(textureSize(texSampler[0], 0));
    return clamp(at, pp.source_uv.zw + half_texel, pp.source_uv.zw + pp.source_uv.xy - half_texel);
}

// The eye's own picture, in the source color space before gamma conversion.
vec4 base(vec2 at) {
    vec4 result = textureLod(texSampler[0], inside(at), 0.0);
    if (pp.srgb_input && !pp.hdr) {
        result.rgb = mix(pow(max(result.rgb + b, 0.0) / a, vec3(2.4)), result.rgb / d,
                         lessThan(result.rgb, vec3(d * cutoff)));
    }
    return result;
}

// The overlay drawn over a colour of the eye's picture.
vec3 overlaid(vec2 at, vec3 rgb) {
    if (pp.has_overlay) {
        vec2 overlay_at = inside(at) * pp.overlay_uv.xy + pp.overlay_uv.zw;
        if (all(greaterThanEqual(overlay_at, vec2(0.0))) &&
            all(lessThanEqual(overlay_at, vec2(1.0)))) {
            vec4 over = textureLod(texSampler[1], overlay_at, 0.0);
            rgb = over.rgb + rgb * (1.0 - clamp(over.a, 0.0, 1.0));
        }
    }
    return rgb;
}

// Compose in the source color space before gamma conversion, including sharpening taps.
vec4 composed(vec2 at) {
    vec4 result = base(at);
    result.rgb = overlaid(at, result.rgb);
    return result;
}

vec3 shown(vec2 at) {
    vec3 rgb = composed(at).rgb;
    return pp.hdr ? rgb : gamma(rgb);
}

// Morphological anti-aliasing (MLAA), on the eye's picture as it is shown: a pixel on an edge
// is found by its contrast with its neighbours; the edge is followed both ways to where it
// ends, and the shape it makes there (which end is nearer, and whether this pixel is on the
// side that sticks out) says how much of the pixel a straight line along the edge would leave
// to the other side: that much of the neighbour across the edge is mixed in.
const float EdgeShare = 0.125;  // The least contrast that makes an edge, of the brightest pixel,
const float EdgeLeast = 0.05;   // and as such.
const float Speck = 0.375;      // How much a pixel unlike all its neighbours is evened out.
const int Steps = 8;            // How far an edge is followed, in pixels a step.
const float StepLength[Steps] = float[Steps](1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0, 12.0);

float Luma(vec3 rgb) {
    const float light = dot(max(rgb, vec3(0.0)), vec3(0.299, 0.587, 0.114));
    return pp.hdr ? sqrt(light / (1.0 + light)) : light;
}

float LumaAt(vec2 at) {
    const vec3 rgb = base(at).rgb;
    return Luma(pp.hdr ? rgb : gamma(rgb));
}

// The share of the neighbour across the edge that belongs into the pixel at `at` (0: no edge
// there), and where that neighbour is.
float Morphological(vec2 at, vec2 texel, out vec2 other_at) {
    other_at = at;
    const float m = LumaAt(at);
    const float n = LumaAt(at - vec2(0.0, texel.y));
    const float s = LumaAt(at + vec2(0.0, texel.y));
    const float w = LumaAt(at - vec2(texel.x, 0.0));
    const float e = LumaAt(at + vec2(texel.x, 0.0));
    const float brightest = max(max(max(n, s), max(w, e)), m);
    const float darkest = min(min(min(n, s), min(w, e)), m);
    const float contrast = brightest - darkest;
    if (contrast < max(EdgeLeast, brightest * EdgeShare)) {
        return 0.0;
    }
    const float nw = LumaAt(at + vec2(-texel.x, -texel.y));
    const float ne = LumaAt(at + vec2(texel.x, -texel.y));
    const float sw = LumaAt(at + vec2(-texel.x, texel.y));
    const float se = LumaAt(at + vec2(texel.x, texel.y));

    // Which way the edge runs: along the rows if brightness changes more from row to row.
    const float across_rows = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) +
                              abs(ne + se - 2.0 * e);
    const float across_columns = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) +
                                 abs(sw + se - 2.0 * s);
    const bool along_rows = across_rows >= across_columns;
    const float before = along_rows ? n : w;
    const float after = along_rows ? s : e;
    const bool steeper_before = abs(before - m) >= abs(after - m);
    const float rise = max(abs(before - m), abs(after - m)) * 0.25;
    const float side = steeper_before ? -1.0 : 1.0;
    const float middle = 0.5 * ((steeper_before ? before : after) + m);
    const bool centre_below = m < middle;

    // From half a pixel towards that neighbour, where both sides of the edge are read at once,
    // along the edge to where it stops being one.
    const vec2 across = along_rows ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    const vec2 along = along_rows ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    const vec2 start = at + across * (side * 0.5);
    float back = 0.0;
    float forth = 0.0;
    float end_back = 0.0;
    float end_forth = 0.0;
    bool done_back = false;
    bool done_forth = false;
    for (int i = 0; i < Steps; ++i) {
        if (!done_back) {
            back += StepLength[i];
            end_back = LumaAt(start - along * back) - middle;
            done_back = abs(end_back) >= rise;
        }
        if (!done_forth) {
            forth += StepLength[i];
            end_forth = LumaAt(start + along * forth) - middle;
            done_forth = abs(end_forth) >= rise;
        }
        if (done_back && done_forth) {
            break;
        }
    }
    // The nearer end decides, if the edge turns there the way that makes this pixel the one
    // that sticks out: the line from that end leaves this much of the pixel to the other side.
    const bool nearer_back = back < forth;
    const float nearer = min(back, forth);
    const bool sticks_out = ((nearer_back ? end_back : end_forth) < 0.0) != centre_below;
    const float by_edge = sticks_out ? 0.5 - nearer / (back + forth) : 0.0;

    // A pixel unlike everything around it is evened out a little whatever the edge says.
    const float around = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;
    float speck = clamp(abs(around - m) / contrast, 0.0, 1.0);
    speck = speck * speck * (3.0 - 2.0 * speck);
    other_at = at + across * side;
    return clamp(max(by_edge, speck * speck * Speck) * pp.mlaa, 0.0, 1.0);
}

void main() {
    vec2 source = uv * pp.source_uv.xy + pp.source_uv.zw;
    vec4 color_linear = composed(source);
    bool smoothed = false;
    if (pp.mlaa > 0.0) {
        // The eye's picture mixed with the neighbour across an edge (in linear light, as the
        // pixel's own share of each side), then the overlay over that as before.
        vec2 other_at;
        const float share =
            Morphological(source, 1.0 / vec2(textureSize(texSampler[0], 0)), other_at);
        if (share > 0.0) {
            const vec3 mixed = mix(base(source).rgb, base(other_at).rgb, share);
            color_linear.rgb = overlaid(source, mixed);
            smoothed = true;
        }
    }
    vec3 here = pp.hdr ? color_linear.rgb : gamma(color_linear.rgb);
    if (pp.sharpen > 0.0 && !smoothed) {
        // Contrast adaptive sharpening: a pixel is pushed away from the four next to it, the
        // more the less they differ already, and never beyond black or white.
        vec2 texel = 1.0 / vec2(textureSize(texSampler[0], 0));
        vec3 above = shown(source - vec2(0.0, texel.y));
        vec3 below = shown(source + vec2(0.0, texel.y));
        vec3 left = shown(source - vec2(texel.x, 0.0));
        vec3 right = shown(source + vec2(texel.x, 0.0));
        vec3 darkest = min(min(min(above, below), min(left, right)), here);
        vec3 brightest = max(max(max(above, below), max(left, right)), here);
        vec3 room = sqrt(clamp(min(darkest, 1.0 - brightest) / max(brightest, vec3(1e-5)),
                               0.0, 1.0));
        vec3 weight = room * (-1.0 / mix(8.0, 5.0, clamp(pp.sharpen, 0.0, 1.0)));
        here = clamp(((above + below + left + right) * weight + here) / (1.0 + 4.0 * weight),
                     0.0, 1.0);
    }
    if (!pp.hdr && pp.dither_quantum > 0.0) {
        float noise = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy),
                                                vec2(0.06711056, 0.00583715)))) - 0.5;
        vec3 room = clamp(min(here, 1.0 - here) / pp.dither_quantum, 0.0, 1.0);
        here = clamp(here + room * noise * pp.dither_quantum, 0.0, 1.0);
    }
    if (pp.linear_out && !pp.hdr) {
        // Back to linear light, by the curve the target encodes with.
        here = mix(pow((here + 0.055) / 1.055, vec3(2.4)), here / 12.92,
                   lessThanEqual(here, vec3(0.04045)));
    }
    color = vec4(here, color_linear.a);
}

#pragma once

#include <vector>

// Every game option the launcher edits, as data: adding one is one entry here.
// Values and defaults mirror pc_port/config.cfg and pc_config.c.

enum class OptKind
{
    Choice,     // fixed list of values
    Check,      // "1" / "0"
    Integer,    // spin box
    Decimal,    // spin box, 2 decimals
    Resolution, // width + height pair
};

struct OptChoice
{
    const char* value;
    const char* label;
};

struct OptionDef
{
    const char*            page;
    const char*            key;
    const char*            label;
    OptKind                kind;
    const char*            def;
    std::vector<OptChoice> choices;
    double                 min  = 0.0;
    double                 max  = 0.0;
    double                 step = 1.0;
    const char*            tip  = "";
};

inline const std::vector<OptionDef>& OptionTable()
{
    static const std::vector<OptionDef> table = {
        // Display
        { "Display", "fullscreen", "Display mode", OptKind::Choice, "0",
          { { "0", "Windowed" }, { "1", "Fullscreen" }, { "2", "Borderless" } } },
        { "Display", "width", "Resolution", OptKind::Resolution, "640", {} },
        { "Display", "vsync", "VSync", OptKind::Check, "0", {} },
        { "Display", "fps_cap", "FPS limit", OptKind::Choice, "30",
          { { "0", "Unlimited" }, { "30", "30 (original)" }, { "60", "60" }, { "120", "120" }, { "240", "240" } } },
        { "Display", "renderer", "Renderer", OptKind::Choice, "gl",
          { { "gl", "OpenGL" }, { "gles", "OpenGL ES (ANGLE)" }, { "d3d11", "Direct3D 11 (ANGLE)" },
            { "vulkan", "Vulkan (ANGLE)" }, { "warp", "WARP (software D3D)" }, { "software", "Software" } },
          0, 0, 1, "The ANGLE backends need libEGL.dll / libGLESv2.dll next to the game." },
        { "Display", "widescreen_mode", "Widescreen (Hor+)", OptKind::Check, "1", {} },
        { "Display", "menu_pillarbox", "Pillarbox menus", OptKind::Check, "1", {} },
        { "Display", "scaling", "Upscaling", OptKind::Choice, "2",
          { { "0", "Integer" }, { "1", "Nearest neighbour" }, { "2", "Bilinear" }, { "3", "Sharp bilinear" } } },

        // Graphics
        { "Graphics", "use_pgxp", "PGXP (precise geometry)", OptKind::Check, "0", {},
          0, 0, 1, "Removes PSX vertex wobble and texture warping. Needed for the best fog and flashlight." },
        { "Graphics", "texture_filter", "Texture filtering", OptKind::Choice, "0",
          { { "0", "Off" }, { "1", "Bilinear" }, { "2", "Trilinear" }, { "3", "Anisotropic 2x" },
            { "4", "Anisotropic 4x" }, { "5", "Anisotropic 8x" }, { "6", "Anisotropic 16x" } } },
        { "Graphics", "dithering", "PSX dithering", OptKind::Check, "1", {} },
        { "Graphics", "msaa", "Antialiasing", OptKind::Choice, "0",
          { { "0", "Off" }, { "2", "MSAA 2x" }, { "4", "MSAA 4x" }, { "8", "MSAA 8x" } } },
        { "Graphics", "post_process", "Post effect", OptKind::Choice, "0",
          { { "0", "Off" }, { "1", "CRT" }, { "2", "Scanlines" }, { "3", "Vignette" }, { "4", "Color grade" },
            { "5", "Film grain" }, { "6", "Sharpen" }, { "7", "PSX retro" }, { "8", "Cinematic" } } },
        { "Graphics", "tonemap", "Tone mapping", OptKind::Choice, "0",
          { { "0", "Off" }, { "1", "Reinhard" }, { "2", "ACES" }, { "3", "Filmic" } } },
        { "Graphics", "preload_chunks", "Preload map chunks", OptKind::Check, "1", {} },
        { "Graphics", "resident_textures", "Keep map textures resident", OptKind::Check, "1", {} },
        { "Graphics", "exterior_full_draw", "Streets without pop-in", OptKind::Check, "1", {}, 0, 0, 1,
          "Outdoors, every street block stays loaded and everything up to the draw distance is drawn; the fog "
          "covers the edge. Needs the two options above." },
        { "Lighting & Fog", "sky_light", "Sky light (soft daytime shadows)", OptKind::Check, "1", {}, 0, 0, 1,
          "A pale light from high above the mist gives the streets soft shadows while the flashlight is off. "
          "Strength, softness and direction: quick options (F10), Fog Layers." },
        { "Graphics", "whole_map_exteriors", "Whole town at once", OptKind::Check, "0", {}, 0, 0, 1,
          "Outdoors, the whole map is streamed, textured and drawn, without the draw distance limit. Heavy; "
          "needs Preload map chunks and Keep map textures resident." },
        { "Graphics", "disable_culling", "Extended draw distance", OptKind::Check, "1", {} },
        { "Graphics", "draw_distance_pct", "Draw distance (%)", OptKind::Integer, "100", {}, 100, 200, 10 },
        { "Graphics", "bullet_decals", "Bullet decals", OptKind::Check, "0", {} },

        // Lighting & fog
        { "Lighting & Fog", "flashlight_mode", "Flashlight", OptKind::Choice, "0",
          { { "0", "Classic (PSX per-vertex)" }, { "1", "Classic + shadows" }, { "2", "Modern" },
            { "3", "Modern + shadows" } } },
        { "Lighting & Fog", "flashlight_replace", "Per-pixel beam replaces the PSX light", OptKind::Check, "1", {},
          0, 0, 1, "Per-pixel modes only. Vertices keep the room's ambient light, the beam is the only flashlight "
                   "light; the intro lighter becomes a flickering light at its flame." },
        { "Lighting & Fog", "flashlight_holder_fill", "Light on Harry (fill)", OptKind::Decimal, "0.10", {}, 0, 1,
          0.01, "Per-pixel beam only. All-round light on Harry, as a share of the beam strength." },
        { "Lighting & Fog", "flashlight_holder_bounce", "Light on Harry (bounce)", OptKind::Decimal, "0.15", {}, 0, 1,
          0.01, "Per-pixel beam only. Light thrown back onto Harry from where the beam lands." },
        { "Lighting & Fog", "flashlight_intensity", "Flashlight brightness", OptKind::Decimal, "1.20", {}, 0, 3, 0.1 },
        { "Lighting & Fog", "flashlight_size", "Flashlight cone size", OptKind::Decimal, "3.00", {}, 0, 3, 0.1 },
        { "Lighting & Fog", "shadow_resolution", "Shadow resolution", OptKind::Choice, "2048",
          { { "256", "256" }, { "512", "512" }, { "1024", "1024" }, { "2048", "2048" }, { "4096", "4096" },
            { "8192", "8192" } } },
        { "Lighting & Fog", "fog_mode", "Fog", OptKind::Choice, "1",
          { { "0", "PSX (per vertex)" }, { "1", "Volumetric (per pixel)" } },
          0, 0, 1, "Volumetric keeps the PSX visibility at street level and adds drifting mist, a ground layer "
                   "and a fogged backdrop instead of a flat wall." },
        { "Lighting & Fog", "fog_strength", "Fog density", OptKind::Decimal, "1.00", {}, 0, 2, 0.05 },
        { "Lighting & Fog", "fog_wisps", "Fog wisps", OptKind::Decimal, "1.00", {}, 0, 2, 0.05,
          "Volumetric fog only. Drifting mist: denser and thinner patches, brighter wisps and a ground "
          "layer. 0 = smooth fog." },
        { "Lighting & Fog", "fog_veil", "Fog density below 1", OptKind::Choice, "0",
          { { "0", "Thinner mist (nothing shows through full fog)" },
            { "1", "Veil (distant geometry keeps some colour)" } } },
        { "Lighting & Fog", "fog_quality", "Fog quality", OptKind::Choice, "6",
          { { "4", "Low (4 samples)" }, { "6", "Medium (6)" }, { "8", "High (8)" } } },
        { "Lighting & Fog", "weather_sim_hz", "Weather simulation", OptKind::Choice, "60",
          { { "30", "30 Hz (original)" }, { "60", "60 Hz" } } },
        { "Lighting & Fog", "weather_style", "Snow & rain", OptKind::Choice, "2",
          { { "0", "PSX exact" }, { "1", "Sub-pixel, fogged, hidden by ceilings" },
            { "2", "Soft flakes and thin rain streaks" } } },

        // Gameplay
        { "Gameplay", "skip_intros", "Intros", OptKind::Choice, "0",
          { { "0", "Show" }, { "1", "Straight to main menu" }, { "2", "Straight into the game" } } },
        { "Gameplay", "uncensored", "Uncensored", OptKind::Check, "0", {} },
        { "Gameplay", "attract_demos", "Attract-mode demos", OptKind::Check, "1", {} },
        { "Gameplay", "text_size", "Text size (%)", OptKind::Integer, "100", {}, 100, 150, 5 },
        { "Gameplay", "pc_port_credits", "PC port credits", OptKind::Check, "1", {} },

        // Audio
        { "Audio", "audio_output", "Audio output", OptKind::Choice, "auto",
          { { "auto", "Automatic" }, { "stereo", "Stereo" }, { "quad", "Quad" }, { "51", "5.1" }, { "71", "7.1" },
            { "hrtf", "Headphones (HRTF)" } } },

        // Debug
        { "Debug", "enable_debug_log", "Write SilentHill.log", OptKind::Check, "0", {} },
        { "Debug", "show_console", "External console window", OptKind::Check, "0", {} },
        { "Debug", "allow_debug_controls", "Debug keys", OptKind::Check, "0", {} },
    };
    return table;
}

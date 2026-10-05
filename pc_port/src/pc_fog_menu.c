/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Fog pages of the quick options overlay (pc_quick_options.c, ROW_FOG):
 * every volumetric fog knob live in game, saved to config.cfg as it changes.
 * Page 0 "Fog": the look; page 1 "Fog Layers": ground mist, wisps and clouds
 * by height; page 2 "Fog Stages": density by distance and the odds and ends,
 * plus a reset; page 3 "Sky Light" and page 4 "Light Shadows": the shadows
 * of the sky light and of the flashlight and lighter. The values themselves live in pc_fog_shape.c
 * (and g_PcConfig / the renderer for the older keys). */

#include "game.h"

#include <stdio.h>
#include <string.h>

#include "pc_config.h"

extern float g_PsyX_FogStrength;
extern float g_PcFogContrast, g_PcFogGlow, g_PcFogGround, g_PcFogGroundH, g_PcFogWispSize;
extern float g_PcFogFollow, g_PcFogFullMul, g_PcFogClouds, g_PcFogMotionBlur;
extern float g_PcFogCloudH, g_PcFogCloudSize, g_PcFogWispsHigh;
extern float g_PcSkyLight, g_PcSkyLightStrength, g_PcSkyLightSoft, g_PcSkyLightTilt, g_PcSkyLightAzimuth;
extern float g_PcSkyLightFade;
extern float g_PcFlShadow, g_PcFlShadowStrength, g_PcFlShadowSoft, g_PcFlShadowFade;
extern float g_PcLtShadow, g_PcLtShadowStrength, g_PcLtShadowSoft, g_PcLtShadowFade, g_PcHarryShadow;
extern float g_PcShadowGapFill;
extern void  Pc_SkyLight_Reset(void);
extern float g_PcFogStageEnd1, g_PcFogStageEnd2, g_PcFogStageD1, g_PcFogStageD2, g_PcFogStageD3;
extern void  Pc_FogShape_Reset(void);

enum
{
    FM_FLOAT = 0, /* *var in [lo, hi] by step */
    FM_MODE,      /* fog_mode 0/1 */
    FM_STRENGTH,  /* fog_strength: renderer + config */
    FM_WISPS,     /* g_PcConfig.fogWisps */
    FM_VEIL,      /* g_PcConfig.fogVeil */
    FM_QUALITY,   /* g_PcConfig.fogQuality 4/6/8 */
    FM_RESET,     /* confirm only */
    FM_TOGGLE,    /* *var 0/1, Off/On */
    FM_WEATHER,   /* g_PcConfig.weatherStyle 0..2 */
    FM_EXTERIOR   /* exterior_full_draw / whole_map_exteriors: 0 original, 1 full reach, 2 whole map */
};

typedef struct
{
    const char* name;
    int         kind;
    const char* key;
    float*      var;
    float       lo;
    float       hi;
    float       step;
    const char* fmt;
} FogMenuRow;

static const FogMenuRow s_fogPage0[] = {
    { "Fog",                  FM_MODE,     "fog_mode",          NULL,             0.0f, 1.0f,   1.0f,  NULL },
    { "Fog Density",          FM_STRENGTH, "fog_strength",      NULL,             0.1f, 2.0f,   0.05f, "%.2f" },
    { "Wisps",                FM_WISPS,    "fog_wisps",         NULL,             0.0f, 2.0f,   0.1f,  "%.1f" },
    { "Wisp Size",            FM_FLOAT,    "fog_size",          &g_PcFogWispSize, 4.0f, 30.0f,  1.0f,  "%.0f m" },
    { "Wisp Contrast",        FM_FLOAT,    "fog_contrast",      &g_PcFogContrast, 0.0f, 0.95f,  0.05f, "%.2f" },
    { "Mist Brightness",      FM_FLOAT,    "fog_glow",          &g_PcFogGlow,     0.0f, 0.5f,   0.02f, "%.2f" },
    { "Follow Camera",        FM_FLOAT,    "fog_follow",        &g_PcFogFollow,   0.0f, 1.0f,   0.05f, "%.2f" },
    { "Mist Motion Blur",     FM_FLOAT,    "fog_motion_blur",   &g_PcFogMotionBlur, 0.0f, 0.9f, 0.05f, "%.2f" },
};

/* The layers by height: ground mist at the street, the wisps, the clouds. */
static const FogMenuRow s_fogPage1[] = {
    { "Ground Mist",          FM_FLOAT,    "fog_ground",        &g_PcFogGround,   0.0f, 4.0f,   0.1f,  "%.1f" },
    { "Ground Mist Height",   FM_FLOAT,    "fog_ground_height", &g_PcFogGroundH,  0.1f, 2.0f,   0.05f, "%.2f m" },
    { "Wisps Up High",        FM_FLOAT,    "fog_wisps_high",    &g_PcFogWispsHigh, 0.0f, 1.0f,  0.05f, "%.2f" },
    { "Clouds",               FM_FLOAT,    "fog_clouds",        &g_PcFogClouds,   0.0f, 1.0f,   0.05f, "%.2f" },
    { "Cloud Height",         FM_FLOAT,    "fog_cloud_height",  &g_PcFogCloudH,   0.0f, 12.0f,  0.5f,  "%.1f m" },
    { "Cloud Size",           FM_FLOAT,    "fog_cloud_size",    &g_PcFogCloudSize, 2.0f, 10.0f, 0.5f,  "%.1fx" },
};

/* The sky light: a pale light from above the mist, soft daytime shadows. */
static const FogMenuRow s_fogPage3[] = {
    { "Sky Light",            FM_TOGGLE,   "sky_light",         &g_PcSkyLight,    0.0f, 1.0f,   1.0f,  NULL },
    { "Sky Shadow Strength",  FM_FLOAT,    "sky_light_strength", &g_PcSkyLightStrength, 0.0f, 0.8f, 0.05f, "%.2f" },
    { "Sky Shadow Softness",  FM_FLOAT,    "sky_light_soft",    &g_PcSkyLightSoft, 1.0f, 16.0f, 1.0f,  "%.0f" },
    { "Sky Shadow Length",    FM_FLOAT,    "sky_light_fade",    &g_PcSkyLightFade, 0.0f, 20.0f, 0.5f,  "%.1f m" },
    { "Sky Light Angle",      FM_FLOAT,    "sky_light_tilt",    &g_PcSkyLightTilt, 0.0f, 60.0f, 5.0f,  "%.0f deg" },
    { "Sky Light Direction",  FM_FLOAT,    "sky_light_azimuth", &g_PcSkyLightAzimuth, 0.0f, 360.0f, 15.0f, "%.0f deg" },
};

/* The hand lights' shadows. Length 0 = the shadow never fades out. */
static const FogMenuRow s_fogPage4[] = {
    { "Harry Casts Shadows",  FM_TOGGLE,   "harry_shadow",      &g_PcHarryShadow, 0.0f, 1.0f,   1.0f,  NULL },
    { "Close Model Gaps",     FM_FLOAT,    "shadow_gap_fill",   &g_PcShadowGapFill, 0.0f, 2.0f, 1.0f,  "%.0f" },
    { "Flashlight Shadows",   FM_TOGGLE,   "flashlight_shadow", &g_PcFlShadow,    0.0f, 1.0f,   1.0f,  NULL },
    { "Flashlight Strength",  FM_FLOAT,    "flashlight_shadow_strength", &g_PcFlShadowStrength, 0.0f, 1.0f, 0.05f, "%.2f" },
    { "Flashlight Softness",  FM_FLOAT,    "flashlight_shadow_soft", &g_PcFlShadowSoft, 0.0f, 20.0f, 1.0f, "%.0f cm" },
    { "Flashlight Length",    FM_FLOAT,    "flashlight_shadow_fade", &g_PcFlShadowFade, 0.0f, 10.0f, 0.25f, "%.2f m" },
    { "Lighter Shadows",      FM_TOGGLE,   "lighter_shadow",    &g_PcLtShadow,    0.0f, 1.0f,   1.0f,  NULL },
    { "Lighter Strength",     FM_FLOAT,    "lighter_shadow_strength", &g_PcLtShadowStrength, 0.0f, 1.0f, 0.05f, "%.2f" },
    { "Lighter Softness",     FM_FLOAT,    "lighter_shadow_soft", &g_PcLtShadowSoft, 0.0f, 20.0f, 1.0f, "%.0f cm" },
    { "Lighter Length",       FM_FLOAT,    "lighter_shadow_fade", &g_PcLtShadowFade, 0.0f, 10.0f, 0.25f, "%.2f m" },
};

static const FogMenuRow s_fogPage2[] = {
    { "Stage 1 Ends At",      FM_FLOAT,    "fog_stage_end1",    &g_PcFogStageEnd1, 1.0f, 60.0f,  1.0f,  "%.0f m" },
    { "Stage 2 Ends At",      FM_FLOAT,    "fog_stage_end2",    &g_PcFogStageEnd2, 2.0f, 120.0f, 1.0f,  "%.0f m" },
    { "Stage 1 Density",      FM_FLOAT,    "fog_stage1",        &g_PcFogStageD1,   0.1f, 8.0f,   0.05f, "%.2f" },
    { "Stage 2 Density",      FM_FLOAT,    "fog_stage2",        &g_PcFogStageD2,   0.1f, 8.0f,   0.05f, "%.2f" },
    { "Stage 3 Density",      FM_FLOAT,    "fog_stage3",        &g_PcFogStageD3,   0.1f, 8.0f,   0.05f, "%.2f" },
    { "Full Fog At",          FM_FLOAT,    "fog_full",          &g_PcFogFullMul,   0.5f, 6.0f,   0.25f, "%.2fx" },
    { "Exterior Map",         FM_EXTERIOR, "exterior_full_draw", NULL,             0.0f, 2.0f,   1.0f,  NULL },
    { "Density Below 1",      FM_VEIL,     "fog_veil",          NULL,              0.0f, 1.0f,   1.0f,  NULL },
    { "Snow & Rain",          FM_WEATHER,  "weather_style",     NULL,              0.0f, 2.0f,   1.0f,  NULL },
    { "Quality",              FM_QUALITY,  "fog_quality",       NULL,              4.0f, 8.0f,   2.0f,  NULL },
    { "Reset Fog to Defaults", FM_RESET,   NULL,                NULL,              0.0f, 0.0f,   0.0f,  NULL },
};

#define FOG_ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const FogMenuRow* Pc_FogMenu_Table(int page, int* count)
{
    switch (page)
    {
        case 0:  *count = FOG_ROWS(s_fogPage0); return s_fogPage0;
        case 1:  *count = FOG_ROWS(s_fogPage1); return s_fogPage1;
        case 2:  *count = FOG_ROWS(s_fogPage2); return s_fogPage2;
        case 3:  *count = FOG_ROWS(s_fogPage3); return s_fogPage3;
        case 4:  *count = FOG_ROWS(s_fogPage4); return s_fogPage4;
        default: *count = 0; return NULL;
    }
}

static const FogMenuRow* Pc_FogMenu_Row(int page, int i)
{
    int                n;
    const FogMenuRow*  rows = Pc_FogMenu_Table(page, &n);

    return (rows != NULL && i >= 0 && i < n) ? &rows[i] : NULL;
}

int Pc_FogMenu_Count(int page)
{
    int n;

    Pc_FogMenu_Table(page, &n);
    return n;
}

int Pc_FogMenu_IsAction(int page, int i)
{
    const FogMenuRow* r = Pc_FogMenu_Row(page, i);

    return r != NULL && r->kind == FM_RESET;
}

const char* Pc_FogMenu_Name(int page, int i)
{
    const FogMenuRow* r = Pc_FogMenu_Row(page, i);

    return r ? r->name : "";
}

const char* Pc_FogMenu_Label(int page, int i, char* buf, int n)
{
    const FogMenuRow* r = Pc_FogMenu_Row(page, i);

    buf[0] = '\0';
    if (r == NULL)
    {
        return buf;
    }
    switch (r->kind)
    {
        case FM_MODE:     snprintf(buf, (size_t)n, "%s", g_PcConfig.fogMode ? "Volumetric" : "PSX"); break;
        case FM_STRENGTH: snprintf(buf, (size_t)n, r->fmt, g_PsyX_FogStrength); break;
        case FM_WISPS:    snprintf(buf, (size_t)n, r->fmt, g_PcConfig.fogWisps); break;
        case FM_VEIL:     snprintf(buf, (size_t)n, "%s", g_PcConfig.fogVeil ? "Veil" : "Thinner Mist"); break;
        case FM_QUALITY:  snprintf(buf, (size_t)n, "%d", g_PcConfig.fogQuality); break;
        case FM_RESET:    break;
        case FM_TOGGLE:   snprintf(buf, (size_t)n, "%s", (*r->var > 0.5f) ? "On" : "Off"); break;
        case FM_EXTERIOR:
            snprintf(buf, (size_t)n, "%s", g_PcConfig.wholeMapExteriors ? "Whole Map"
                                           : (g_PcConfig.exteriorFullDraw ? "Full Draw Reach" : "Original"));
            break;
        case FM_WEATHER:
        {
            static const char* const names[3] = { "PSX", "Sub-pixel", "Soft" };
            int w = g_PcConfig.weatherStyle;

            snprintf(buf, (size_t)n, "%s", names[(w < 0) ? 0 : ((w > 2) ? 2 : w)]);
            break;
        }
        default:          snprintf(buf, (size_t)n, r->fmt, *r->var); break;
    }
    return buf;
}

static float FogMenuStep(float v, const FogMenuRow* r, int dir)
{
    /* Fine-stepped rows that reach past 2 (the stage densities) step coarser up there. */
    const float step = (r->step < 0.25f && r->hi > 2.0f && (dir > 0 ? v >= 1.999f : v > 2.001f)) ? 0.25f : r->step;

    v += (dir < 0 ? -step : step);
    /* Snap to the step grid so repeated presses do not drift. */
    v = (float)((int)(v / step + (v >= 0.0f ? 0.5f : -0.5f))) * step;
    return (v < r->lo) ? r->lo : ((v > r->hi) ? r->hi : v);
}

static void FogMenuSave(const char* key, float v, const char* fmt)
{
    char buf[24];

    snprintf(buf, sizeof(buf), fmt, v);
    PcConfig_SaveKeyValue(key, buf);
}

void Pc_FogMenu_Adjust(int page, int i, int dir)
{
    const FogMenuRow* r = Pc_FogMenu_Row(page, i);

    if (r == NULL || dir == 0)
    {
        return;
    }
    switch (r->kind)
    {
        case FM_MODE:
            g_PcConfig.fogMode = !g_PcConfig.fogMode;
            PcConfig_SaveKeyValue(r->key, g_PcConfig.fogMode ? "1" : "0");
            break;
        case FM_STRENGTH:
            g_PsyX_FogStrength    = FogMenuStep(g_PsyX_FogStrength, r, dir);
            g_PcConfig.fogStrength = g_PsyX_FogStrength;
            FogMenuSave(r->key, g_PsyX_FogStrength, "%.2f");
            break;
        case FM_WISPS:
            g_PcConfig.fogWisps = FogMenuStep(g_PcConfig.fogWisps, r, dir);
            FogMenuSave(r->key, g_PcConfig.fogWisps, "%.2f");
            break;
        case FM_VEIL:
            g_PcConfig.fogVeil = !g_PcConfig.fogVeil;
            PcConfig_SaveKeyValue(r->key, g_PcConfig.fogVeil ? "1" : "0");
            break;
        case FM_QUALITY:
        {
            int q = g_PcConfig.fogQuality + (dir < 0 ? -2 : 2);
            char buf[8];

            g_PcConfig.fogQuality = (q < 4) ? 4 : ((q > 8) ? 8 : q);
            snprintf(buf, sizeof(buf), "%d", g_PcConfig.fogQuality);
            PcConfig_SaveKeyValue(r->key, buf);
            break;
        }
        case FM_RESET:
            break;
        case FM_TOGGLE:
            *r->var = (*r->var > 0.5f) ? 0.0f : 1.0f;
            PcConfig_SaveKeyValue(r->key, (*r->var > 0.5f) ? "1" : "0");
            break;
        case FM_EXTERIOR:
        {
            /* Original streaming window -> everything within the draw reach ->
             * the whole town (outdoor cells; needs preload + resident textures). */
            int m = g_PcConfig.wholeMapExteriors ? 2 : (g_PcConfig.exteriorFullDraw ? 1 : 0);

            m = (m + (dir < 0 ? 2 : 1)) % 3;
            g_PcConfig.exteriorFullDraw  = (m >= 1);
            g_PcConfig.wholeMapExteriors = (m == 2);
            PcConfig_SaveKeyValue("exterior_full_draw", g_PcConfig.exteriorFullDraw ? "1" : "0");
            PcConfig_SaveKeyValue("whole_map_exteriors", g_PcConfig.wholeMapExteriors ? "1" : "0");
            break;
        }
        case FM_WEATHER:
        {
            int  w = (g_PcConfig.weatherStyle + (dir < 0 ? 2 : 1)) % 3;
            char buf[4];

            g_PcConfig.weatherStyle = w;
            snprintf(buf, sizeof(buf), "%d", w);
            PcConfig_SaveKeyValue(r->key, buf);
            break;
        }
        default:
            *r->var = FogMenuStep(*r->var, r, dir);
            if (r->var == &g_PcFogStageEnd2 && g_PcFogStageEnd2 < g_PcFogStageEnd1 + 1.0f)
            {
                g_PcFogStageEnd2 = g_PcFogStageEnd1 + 1.0f;
            }
            if (r->var == &g_PcFogStageEnd1 && g_PcFogStageEnd2 < g_PcFogStageEnd1 + 1.0f)
            {
                g_PcFogStageEnd2 = g_PcFogStageEnd1 + 1.0f;
                FogMenuSave("fog_stage_end2", g_PcFogStageEnd2, "%.2f");
            }
            FogMenuSave(r->key, *r->var, "%.3f");
            break;
    }
}

/* Confirm: the reset row puts every fog knob back and saves that. */
void Pc_FogMenu_Confirm(int page, int i)
{
    const FogMenuRow* r = Pc_FogMenu_Row(page, i);
    int               p;
    int               k;

    if (r == NULL || r->kind != FM_RESET)
    {
        return;
    }

    Pc_FogShape_Reset();
    Pc_SkyLight_Reset();
    g_PcConfig.fogWisps    = 1.0f;
    g_PcConfig.fogVeil     = 0;
    g_PcConfig.fogQuality  = 6;
    PcConfig_SaveKeyValue("fog_wisps", "1.00");
    PcConfig_SaveKeyValue("fog_veil", "0");
    PcConfig_SaveKeyValue("fog_quality", "6");
    for (p = 0; p < 5; p++)
    {
        for (k = 0; k < Pc_FogMenu_Count(p); k++)
        {
            const FogMenuRow* f = Pc_FogMenu_Row(p, k);

            if (f->kind == FM_FLOAT || f->kind == FM_TOGGLE)
            {
                FogMenuSave(f->key, *f->var, "%.3f");
            }
        }
    }
}

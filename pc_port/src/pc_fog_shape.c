/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Volumetric fog shaping for fog_mode 1 (PsyX_render.cpp fogField /
 * fogVolumetric): the wisp-field parameters, the wind-integrated drift, the
 * full-fog guard distance and what fog_strength < 1 means. Pushed once per
 * frame from Pc_VolumetricFogPush. Q8 units (256 = 1 m), world Y down. Own
 * state only, never the game RNG (demo playback must not change). */

#include "game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/gfx/world.h"
#include "bodyprog/map/map.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_config.h"

extern s_WorldEnvWork g_WorldEnvWork;
extern float          g_PsyX_FogStrength;
extern float          g_PsyX_FogParams[36];

/* Tunables: console commands, the quick options Fog pages (pc_fog_menu.c) and
 * config keys (s_pcFogTunables). fog_wisps scales the first four. */
float g_PcFogContrast = 0.45f; /* fogcontrast: wisp density contrast 0..0.95 */
float g_PcFogGlow     = 0.08f; /* fogglow: dense mist brighter, gaps darker 0..0.5 */
float g_PcFogLit      = 0.22f; /* foglit: top-lit / under-shaded wisp edges 0..0.5 */
float g_PcFogGround   = 1.6f;  /* fogground: ground-mist density at the ground, x the street fog 0..4 */
float g_PcFogGroundH  = 0.6f;  /* fogground 2nd arg: ground-mist scale height, metres 0.1..2 */
float g_PcFogPatch    = 4.0f;  /* ground-mist patch size, metres */
float g_PcFogWispSize = 17.0f; /* fogsize: wisp length along the wind, metres 2..40 */
float g_PcFogWind     = 0.45f; /* fogwind: drift speed, m/s 0..3 */
float g_PcFogWindSnow = 0.25f; /* fogwind 2nd arg: share of the map's snow wind 0..1 */
float g_PcFogFullMul  = 1.75f; /* fogfull: full fog at x * the natural full-fog distance (capped at the draw edge) */
float g_PcFogFollow   = 0.25f; /* fogfollow: share of the camera's travel the mist moves along 0..1 */
/* fogclouds: cloud masses in the mist, 0..1 (a large slow octave). */
float g_PcFogClouds   = 0.5f;
float g_PcFogCloudH    = 2.0f; /* cloud layer starts this high above the ground, m */
float g_PcFogCloudSize = 4.0f; /* cloud cells, x the wisp size */
float g_PcFogWispsHigh = 0.5f; /* share of the wisps kept up in the cloud layer */
/* fogblur: fog motion blur, share of the previous frame a mist pixel keeps
 * per 60 Hz frame (renderer post pass), 0 = off. */
float g_PcFogMotionBlur = 0.9f;
/* Distance stages of the street fog: density 1 up to end 1, density 2 up to
 * end 2, density 3 beyond, with soft 4 m ramps. They multiply fog_strength's
 * density; all three at 1 is the plain fog. The defaults (thin near, dense far)
 * keep the street around Harry readable and hide the distant draw. */
float g_PcFogStageEnd1 = 15.0f;
float g_PcFogStageEnd2 = 18.0f;
float g_PcFogStageD1   = 0.35f;
float g_PcFogStageD2   = 0.7f;
float g_PcFogStageD3   = 2.5f;

typedef struct
{
    const char* key;
    float*      var;
    float       def;
    float       lo;
    float       hi;
} PcFogTunable;

static const PcFogTunable s_pcFogTunables[] = {
    { "fog_contrast",      &g_PcFogContrast,  0.45f, 0.0f, 0.95f },
    { "fog_glow",          &g_PcFogGlow,      0.08f, 0.0f, 0.5f },
    { "fog_lit",           &g_PcFogLit,       0.22f, 0.0f, 0.5f },
    { "fog_ground",        &g_PcFogGround,    1.6f,  0.0f, 4.0f },
    { "fog_ground_height", &g_PcFogGroundH,   0.6f, 0.1f, 2.0f },
    { "fog_size",          &g_PcFogWispSize,  17.0f, 2.0f, 40.0f },
    { "fog_wind",          &g_PcFogWind,      0.45f, 0.0f, 3.0f },
    { "fog_full",          &g_PcFogFullMul,   1.75f,  0.5f, 6.0f },
    { "fog_follow",        &g_PcFogFollow,    0.25f, 0.0f, 1.0f },
    { "fog_clouds",        &g_PcFogClouds,    0.5f,  0.0f, 1.0f },
    { "fog_cloud_height",  &g_PcFogCloudH,    2.0f,  0.0f, 12.0f },
    { "fog_cloud_size",    &g_PcFogCloudSize, 4.0f,  2.0f, 10.0f },
    { "fog_wisps_high",    &g_PcFogWispsHigh, 0.5f,  0.0f, 1.0f },
    { "fog_stage_end1",    &g_PcFogStageEnd1, 15.0f,  1.0f, 60.0f },
    { "fog_stage_end2",    &g_PcFogStageEnd2, 18.0f, 2.0f, 120.0f },
    { "fog_stage1",        &g_PcFogStageD1,   0.35f,  0.1f, 8.0f },
    { "fog_stage2",        &g_PcFogStageD2,   0.7f, 0.1f, 8.0f },
    { "fog_stage3",        &g_PcFogStageD3,   2.5f,  0.1f, 8.0f },
    { "fog_motion_blur",   &g_PcFogMotionBlur, 0.9f, 0.0f, 0.9f },
};

/* Config loader hook (pc_config.c): the fog_* keys owned here. */
int Pc_FogShape_ParseKey(const char* key, const char* value)
{
    size_t i;

    for (i = 0; i < sizeof(s_pcFogTunables) / sizeof(s_pcFogTunables[0]); i++)
    {
        const PcFogTunable* t = &s_pcFogTunables[i];

        if (strcmp(key, t->key) == 0)
        {
            float v = (float)atof(value);

            *t->var = (v < t->lo) ? t->lo : ((v > t->hi) ? t->hi : v);
            return 1;
        }
    }
    return 0;
}

/* Back to the built-in look; the caller persists what it needs. */
void Pc_FogShape_Reset(void)
{
    size_t i;

    for (i = 0; i < sizeof(s_pcFogTunables) / sizeof(s_pcFogTunables[0]); i++)
    {
        *s_pcFogTunables[i].var = s_pcFogTunables[i].def;
    }
}

static float s_drift[3];
static float s_drift2[3];
static float s_patch[3];
static float s_time;
static float s_groundY;
static int   s_groundInit;
static int   s_mapIdx = -2;
static float s_camPrev[3];
static int   s_camInit;

static float FogClampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* CPU twin of the shader's fogStageDepth: staged length from the eye to s (Q8). */
static float FogStageRamp(float s, float a, float w)
{
    const float x = s - (a - 0.5f * w);

    if (x <= 0.0f)
    {
        return 0.0f;
    }
    if (x < w)
    {
        return 0.5f * x * x / w;
    }
    return x - 0.5f * w;
}

static float FogStageDepth(float s, const float* st)
{
    const float w  = st[5];
    const float r1 = FogStageRamp(s, st[0], w) - FogStageRamp(0.0f, st[0], w);
    const float r2 = FogStageRamp(s, st[1], w) - FogStageRamp(0.0f, st[1], w);

    return st[2] * s + (st[3] - st[2]) * r1 + (st[4] - st[3]) * r2;
}

/* nearQ8: fog distance; lightGate: 0 black darkness fog .. 1 light mist;
 * playerY: the player's feet, world Y (Q8). Returns the smoothed ground
 * reference for the ground layer and the rooftop thinning. */
float Pc_FogShapePush(float nearQ8, float lightGate, float playerY)
{
    const float wdx  = 0.70710678f;
    const float wdz  = 0.70710678f;
    const float dt   = (g_DeltaTime > 0) ? (float)g_DeltaTime / 4096.0f : 0.0f;
    const float s    = g_PsyX_FogStrength;
    const float m    = FogClampf(g_PcConfig.fogWisps, 0.0f, 2.0f);
    const int   map  = (g_SavegamePtr != NULL) ? (int)g_SavegamePtr->mapIdx : -1;
    float       near = (nearQ8 > 256.0f) ? nearQ8 : 256.0f;
    float       dS   = 1.0f;
    float       size;
    float       la;
    float       lc;
    float       ly;
    float       vA;
    float       vC;
    float       full;
    float       range;
    float       natural;
    int         i;

    /* New map: restart the drift (small lattice coordinates) and snap the ground. */
    if (map != s_mapIdx)
    {
        s_mapIdx = map;
        for (i = 0; i < 3; i++)
        {
            s_drift[i]  = 0.0f;
            s_drift2[i] = 0.0f;
            s_patch[i]  = 0.0f;
        }
        s_groundInit = 0;
        s_camInit    = 0;
    }

    /* The ground layer follows the player's feet, low-passed (~0.7 s) so kerbs
     * and stairs do not jerk it; it snaps on jumps over 4 m (cuts, teleports). */
    if (!s_groundInit || fabsf(playerY - s_groundY) > 1024.0f)
    {
        s_groundY    = playerY;
        s_groundInit = 1;
    }
    else
    {
        s_groundY += (playerY - s_groundY) * fminf(1.0f, dt * 1.5f);
    }

    /* fog_strength < 1 in LIGHT fog = thinner density: full fog comes later and
     * nothing keeps a veil of its own colour (through the veil the per-poly draw
     * edge at ~61 m showed as streets popping in). Black darkness fog and
     * fog_veil 1 keep the old amount multiplier. */
    if (!g_PcConfig.fogVeil && s < 1.0f)
    {
        dS = 1.0f - FogClampf(lightGate, 0.0f, 1.0f) * (1.0f - FogClampf(s, 0.05f, 1.0f));
    }

    size = FogClampf(sqrtf(near / 3456.0f), 0.7f, 1.5f);
    la   = FogClampf(g_PcFogWispSize, 2.0f, 40.0f) * 256.0f * size;
    lc   = la * 0.39f;
    ly   = la * 0.27f;
    /* The street wisps lie in layers: wide, flat cells (the "Layers" type the
     * earlier wisp-type choice offered, the one that fit the look). */
    la *= 1.3f;
    lc *= 1.3f;
    ly *= 0.45f;

    s_time += dt;
    vA = g_PcFogWind * (1.0f + 0.35f * sinf(s_time * 0.071f));
    vC = g_PcFogWind * 0.25f * sinf(s_time * 0.043f + 1.3f);
    if (g_pMapOverlayHeader != NULL && g_pMapOverlayHeader->windSpeedX != NULL &&
        g_pMapOverlayHeader->windSpeedZ != NULL)
    {
        /* Snow wind, assumed Q12 metres per 30 fps frame. */
        const float px = (float)*g_pMapOverlayHeader->windSpeedX * (30.0f / 4096.0f);
        const float pz = (float)*g_pMapOverlayHeader->windSpeedZ * (30.0f / 4096.0f);

        vA += g_PcFogWindSnow * (px * wdx + pz * wdz);
        vC += g_PcFogWindSnow * (pz * wdx - px * wdz);
    }

    /* q = coord * scale + drift moves the field by -drift: subtract to blow
     * downwind. The detail octave runs at 1.6x the base speed (rolling,
     * shearing wisps); a slow vertical boil on both. */
    s_drift[0]  -= vA * 256.0f * dt / la;
    s_drift[1]  += 0.012f * dt;
    s_drift[2]  -= vC * 256.0f * dt / lc;
    s_drift2[0] -= 0.6f * 2.03f * vA * 256.0f * dt / la;
    s_drift2[1] += 0.05f * dt;
    s_drift2[2] -= 0.6f * 2.11f * vC * 256.0f * dt / lc;

    /* Ground-mist patches: fixed in the world apart from a slow creep with the
     * wind (never with the camera), so Harry walks through them. */
    {
        const float pc = FogClampf(g_PcFogPatch, 1.0f, 20.0f) * 256.0f;

        s_patch[0] -= 0.5f * (vA * wdx - vC * wdz) * 256.0f * dt / pc;
        s_patch[1] -= 0.5f * (vA * wdz + vC * wdx) * 256.0f * dt / pc;
        s_patch[2] += 0.02f * dt;
    }

    /* The mist travels part of the way with the camera. Wisps a few metres
     * across went past at running speed (5.5 m/s is a wisp per second) and the
     * fog churned whenever Harry moved, while standing still it only drifts.
     * Turning is unaffected (the samples are spheres around the eye), and cuts
     * (fixed cameras, teleports) are skipped. The detail octave rides on q, so
     * shifting the base drift moves both. */
    {
        extern float g_PsyX_FogViewToWorld[16];
        const float  cx = g_PsyX_FogViewToWorld[12];
        const float  cy = g_PsyX_FogViewToWorld[13];
        const float  cz = g_PsyX_FogViewToWorld[14];

        if (s_camInit)
        {
            const float dx = cx - s_camPrev[0];
            const float dy = cy - s_camPrev[1];
            const float dz = cz - s_camPrev[2];

            if (dx * dx + dy * dy + dz * dz < 512.0f * 512.0f)
            {
                const float k = FogClampf(g_PcFogFollow, 0.0f, 1.0f);

                s_drift[0] -= k * (dx * wdx + dz * wdz) / la;
                s_drift[1] -= k * dy / ly;
                s_drift[2] -= k * (dz * wdx - dx * wdz) / lc;
            }
        }
        s_camPrev[0] = cx;
        s_camPrev[1] = cy;
        s_camPrev[2] = cz;
        s_camInit    = 1;
    }

    /* Full-fog guard: past F everything is exactly the void. Kept short of where
     * geometry stops: the PSX face cull (culling on) or 80 % of the per-poly far
     * drop (0x79C << 3 = 15584 Q8 x draw_distance_pct, disable_culling); always
     * short of the 255 m backdrop. */
    /* Distance stages; the natural full-fog distance is where the staged depth
     * reaches near / density, which is near / density itself when all three
     * stages are 1. */
    {
        float st[6];
        float target;
        float s;

        st[0] = FogClampf(g_PcFogStageEnd1, 1.0f, 60.0f) * 256.0f;
        st[1] = FogClampf(g_PcFogStageEnd2, g_PcFogStageEnd1 + 1.0f, 120.0f) * 256.0f;
        st[2] = FogClampf(g_PcFogStageD1, 0.1f, 8.0f);
        st[3] = FogClampf(g_PcFogStageD2, 0.1f, 8.0f);
        st[4] = FogClampf(g_PcFogStageD3, 0.1f, 8.0f);
        st[5] = 1024.0f;
        /* u_fogParams[6] = (end 1, end 2, density 1, density 2), [7] = (density 3, ramp) */
        g_PsyX_FogParams[24] = st[0];
        g_PsyX_FogParams[25] = st[1];
        g_PsyX_FogParams[26] = st[2];
        g_PsyX_FogParams[27] = st[3];
        g_PsyX_FogParams[28] = st[4];
        g_PsyX_FogParams[29] = st[5];
        g_PsyX_FogParams[30] = 0.0f; /* [7].z: the cloud amount, set below */
        g_PsyX_FogParams[31] = 0.0f;

        target = near / dS;
        for (s = 0.0f; s < 60000.0f && FogStageDepth(s, st) < target; s += 32.0f)
        {
        }
        natural = s;
    }

    full = FogClampf(g_PcFogFullMul, 0.5f, 6.0f) * natural;
    if (!Pc_WholeMapDrawActive())
    {
        const float cap = g_PcConfig.disableCulling
                              ? 0.8f * 15584.0f *
                                    (float)((g_PcConfig.drawDistancePct <= 0) ? 100 : g_PcConfig.drawDistancePct) /
                                    100.0f
                              : (float)g_WorldEnvWork.fog.farDistance - 64.0f;
        if (full > cap)
        {
            full = cap;
        }
    }
    full  = FogClampf(full, 256.0f, 58752.0f);
    range = FogClampf(1.25f * natural, 256.0f, full);

    g_PsyX_FogParams[0]  = g_PcFogContrast * m;
    g_PsyX_FogParams[1]  = g_PcFogGlow * m;
    g_PsyX_FogParams[2]  = g_PcFogLit * m;
    g_PsyX_FogParams[3]  = g_PcFogGround * m;
    g_PsyX_FogParams[4]  = wdx;
    g_PsyX_FogParams[5]  = wdz;
    g_PsyX_FogParams[6]  = FogClampf(g_PcFogGroundH, 0.1f, 2.0f) * 256.0f;
    g_PsyX_FogParams[7]  = full;
    g_PsyX_FogParams[8]  = 1.0f / la;
    g_PsyX_FogParams[9]  = 1.0f / ly;
    g_PsyX_FogParams[10] = 1.0f / lc;
    g_PsyX_FogParams[11] = range;
    g_PsyX_FogParams[12] = s_drift[0];
    g_PsyX_FogParams[13] = s_drift[1];
    g_PsyX_FogParams[14] = s_drift[2];
    g_PsyX_FogParams[15] = dS;
    g_PsyX_FogParams[16] = s_drift2[0];
    g_PsyX_FogParams[17] = s_drift2[1];
    g_PsyX_FogParams[18] = s_drift2[2];
    g_PsyX_FogParams[19] = (float)((g_PcConfig.fogQuality < 4) ? 4 : ((g_PcConfig.fogQuality > 8) ? 8 : g_PcConfig.fogQuality));
    g_PsyX_FogParams[20] = s_patch[0];
    g_PsyX_FogParams[21] = s_patch[1];
    g_PsyX_FogParams[22] = 1.0f / (FogClampf(g_PcFogPatch, 1.0f, 20.0f) * 256.0f);
    g_PsyX_FogParams[23] = s_patch[2];
    g_PsyX_FogParams[30] = FogClampf(g_PcFogClouds * m, 0.0f, 1.5f);
    /* u_fogParams[8]: the cloud layer (pc_fog_menu.c page "Fog Layers"). */
    g_PsyX_FogParams[32] = 1.0f / FogClampf(g_PcFogCloudSize, 2.0f, 10.0f);
    g_PsyX_FogParams[33] = FogClampf(g_PcFogCloudH, 0.0f, 12.0f) * 256.0f;
    g_PsyX_FogParams[34] = 1024.0f;
    g_PsyX_FogParams[35] = FogClampf(g_PcFogWispsHigh, 0.0f, 1.0f);
    {
        extern float g_PsyX_FogMotionBlur;
        g_PsyX_FogMotionBlur = FogClampf(g_PcFogMotionBlur, 0.0f, 0.9f);
    }
    return s_groundY;
}

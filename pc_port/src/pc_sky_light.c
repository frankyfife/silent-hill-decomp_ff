/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sky light: a pale light from high above the mist that gives the daytime
 * streets soft shadows (renderer: PsyX_render.cpp GR_BuildSkyShadowMatrix and
 * the sky block of the lit tail).
 *
 * In the fog the world has no light of its own direction; without any shadow
 * the streets read flat, every car and fence standing on the ground without
 * touching it. This places a fictitious, nearly overhead light (a sun or moon
 * behind the mist) and lets the flashlight's shadow-map pass draw the scene
 * from it, orthographic, over a square that travels with Harry. The shader
 * darkens what that light cannot see by a modest share, softly; the fog then
 * covers it with distance like everything else.
 *
 * Active only where it makes sense: light fog (day mist, not the darkness
 * falloff), Harry in an outdoor cell of an exterior map (no ceiling over him),
 * and the flashlight off (it owns the shadow map then). Pushed once per frame
 * from Gfx_2dEffectsDraw. Own state only, no game RNG. */

#include "game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/gfx/world.h"
#include "pc_config.h"
#include "sh_log.h"

extern s_WorldEnvWork g_WorldEnvWork;
extern int            Pc_PlayerCellOutdoor(void);

/* Renderer inputs (PsyX_render.cpp), Q8 units. The map is built in WORLD
 * space relative to an anchor near Harry: built from view space, the camera's
 * Q12 rotation rounding moved the texel grid by more than a texel at a world
 * origin hundreds of metres away, and the shadows crawled whenever the camera
 * turned. */
extern int   g_PsyX_SkyLightOn;
extern float g_PsyX_SkyLightDir[3];       /* world: the way the light travels (toward the ground) */
extern float g_PsyX_SkyLightRight[3];     /* world X: the map's roll reference */
extern float g_PsyX_SkyLightDirView[3];   /* the same direction in view space (shader facing term) */
extern float g_PsyX_SkyViewToAnchor[16];  /* view space -> world relative to the anchor, column-major */
extern float g_PsyX_SkyLightParams[4];    /* strength, softness (texels), half size, light gate */
extern int   g_PsyX_ShadowMapSize;        /* the map's resolution (texel grid) */
extern float g_PsyX_SkyShadowFade;        /* view units an occluder's shadow reaches (0 = no fade) */
/* The hand lights' shadows ([0] flashlight, [1] lighter) and who casts. */
extern int   g_PsyX_HandShadowOn[2];
extern float g_PsyX_HandShadowStrength[2];
extern float g_PsyX_HandShadowSoft[2];     /* PCSS lens radius, view units */
extern float g_PsyX_HandShadowFade[2];     /* view units an occluder's shadow reaches (0 = no fade) */
extern int   g_PsyX_ShadowHarryCasts;
extern int   g_PsyX_ShadowDilate;          /* gap fill radius, texels */

/* Tunables (quick options "Sky Light" and "Light Shadows", config keys below). */
float g_PcSkyLight         = 1.0f;  /* sky_light: on/off */
float g_PcSkyLightStrength = 0.3f;  /* sky_light_strength: share of light the shadow takes, 0..0.8 */
float g_PcSkyLightSoft     = 6.0f;  /* sky_light_soft: penumbra radius, shadow texels 1..16 */
float g_PcSkyLightFade     = 6.0f;  /* sky_light_fade: metres behind its caster a shadow fades out over, 0 = no fade */
float g_PcSkyLightTilt     = 25.0f; /* sky_light_tilt: degrees from straight overhead, 0..60 */
float g_PcSkyLightAzimuth  = 40.0f; /* sky_light_azimuth: compass direction it falls toward, degrees */
/* Hand lights: the flashlight's and the lighter's shadows, each on its own.
 * Softness is the size of the light (cm): a bigger light gives a wider
 * penumbra that grows with the distance to the caster. Length: the shadow
 * fades out over this many metres behind its caster (0 = it never fades). */
float g_PcFlShadow         = 1.0f;  /* flashlight_shadow */
float g_PcFlShadowStrength = 0.5f;  /* flashlight_shadow_strength 0..1 */
float g_PcFlShadowSoft     = 10.0f; /* flashlight_shadow_soft, cm 0..20: the width of its vague blur */
float g_PcFlShadowFade     = 2.5f;  /* flashlight_shadow_fade, m 0..10 */
float g_PcLtShadow         = 1.0f;  /* lighter_shadow */
float g_PcLtShadowStrength = 0.6f;  /* lighter_shadow_strength 0..1 */
float g_PcLtShadowSoft     = 6.0f;  /* lighter_shadow_soft, cm 0..20: here the width of its vague blur */
float g_PcLtShadowFade     = 2.5f;  /* lighter_shadow_fade, m 0..10 */
float g_PcHarryShadow      = 1.0f;  /* harry_shadow: Harry casts (every light) */
float g_PcShadowGapFill    = 1.0f;  /* shadow_gap_fill: close the cracks between model parts, texels 0..2 */

typedef struct
{
    const char* key;
    float*      var;
    float       def;
    float       lo;
    float       hi;
} PcSkyTunable;

static const PcSkyTunable s_pcSkyTunables[] = {
    { "sky_light",          &g_PcSkyLight,         1.0f,  0.0f, 1.0f },
    { "sky_light_strength", &g_PcSkyLightStrength, 0.3f,  0.0f, 0.8f },
    { "sky_light_soft",     &g_PcSkyLightSoft,     6.0f,  1.0f, 16.0f },
    { "sky_light_fade",     &g_PcSkyLightFade,     6.0f,  0.0f, 20.0f },
    { "sky_light_tilt",     &g_PcSkyLightTilt,     25.0f, 0.0f, 60.0f },
    { "sky_light_azimuth",  &g_PcSkyLightAzimuth,  40.0f, 0.0f, 360.0f },
    { "flashlight_shadow",          &g_PcFlShadow,         1.0f, 0.0f, 1.0f },
    { "flashlight_shadow_strength", &g_PcFlShadowStrength, 0.5f, 0.0f, 1.0f },
    { "flashlight_shadow_soft",     &g_PcFlShadowSoft,     10.0f, 0.0f, 20.0f },
    { "flashlight_shadow_fade",     &g_PcFlShadowFade,     2.5f, 0.0f, 10.0f },
    { "lighter_shadow",             &g_PcLtShadow,         1.0f, 0.0f, 1.0f },
    { "lighter_shadow_strength",    &g_PcLtShadowStrength, 0.6f,  0.0f, 1.0f },
    { "lighter_shadow_soft",        &g_PcLtShadowSoft,     6.0f,  0.0f, 20.0f },
    { "lighter_shadow_fade",        &g_PcLtShadowFade,     2.5f, 0.0f, 10.0f },
    { "harry_shadow",               &g_PcHarryShadow,      1.0f, 0.0f, 1.0f },
    { "shadow_gap_fill",            &g_PcShadowGapFill,    1.0f, 0.0f, 2.0f },
};

int Pc_SkyLight_ParseKey(const char* key, const char* value)
{
    size_t i;

    for (i = 0; i < sizeof(s_pcSkyTunables) / sizeof(s_pcSkyTunables[0]); i++)
    {
        const PcSkyTunable* t = &s_pcSkyTunables[i];

        if (strcmp(key, t->key) == 0)
        {
            float v = (float)atof(value);

            *t->var = (v < t->lo) ? t->lo : ((v > t->hi) ? t->hi : v);
            return 1;
        }
    }
    return 0;
}

void Pc_SkyLight_Reset(void)
{
    size_t i;

    for (i = 0; i < sizeof(s_pcSkyTunables) / sizeof(s_pcSkyTunables[0]); i++)
    {
        *s_pcSkyTunables[i].var = s_pcSkyTunables[i].def;
    }
}

/* World vector through GsWSMATRIX's rotation into view space. */
static void SkyRotate(const float w[3], float out[3])
{
    int r;

    for (r = 0; r < 3; r++)
    {
        out[r] = ((float)GsWSMATRIX.m[r][0] * w[0] + (float)GsWSMATRIX.m[r][1] * w[1] +
                  (float)GsWSMATRIX.m[r][2] * w[2]) /
                 4096.0f;
    }
}

static void SkyCross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void SkyNormalize(double v[3])
{
    const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (l > 1e-12)
    {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void Pc_SkyLightPush(int flashlightActive)
{
    float  lum;
    float  tilt;
    float  az;
    double R[3][3];
    double cam[3];
    double P[3];
    double f[3];
    double ref[3] = { 1.0, 0.0, 0.0 };
    double s[3];
    double u[3];
    double half;
    double texel;
    double ps;
    double pu;
    int    r;
    int    c;

    /* The hand lights' shadow settings ride along every frame (view units:
     * 256 per metre, 2.56 per cm). */
    g_PsyX_HandShadowOn[0]       = g_PcFlShadow > 0.5f;
    g_PsyX_HandShadowStrength[0] = g_PcFlShadowStrength;
    g_PsyX_HandShadowSoft[0]     = g_PcFlShadowSoft * 2.56f;
    g_PsyX_HandShadowFade[0]     = g_PcFlShadowFade * 256.0f;
    g_PsyX_HandShadowOn[1]       = g_PcLtShadow > 0.5f;
    g_PsyX_HandShadowStrength[1] = g_PcLtShadowStrength;
    g_PsyX_HandShadowSoft[1]     = g_PcLtShadowSoft * 2.56f;
    g_PsyX_HandShadowFade[1]     = g_PcLtShadowFade * 256.0f;
    g_PsyX_ShadowHarryCasts      = g_PcHarryShadow > 0.5f;
    g_PsyX_ShadowDilate          = (int)(g_PcShadowGapFill + 0.5f);
    g_PsyX_SkyShadowFade         = g_PcSkyLightFade * 256.0f;

    /* Light mist only: the same gate the volumetric fog uses for its wisps. */
    lum = (0.299f * g_WorldEnvWork.fog.color.r + 0.587f * g_WorldEnvWork.fog.color.g +
           0.114f * g_WorldEnvWork.fog.color.b) /
          255.0f;
    lum = (lum - 0.08f) / (0.25f - 0.08f);
    lum = (lum < 0.0f) ? 0.0f : ((lum > 1.0f) ? 1.0f : lum);

    g_PsyX_SkyLightOn = (g_PcSkyLight > 0.5f && g_PcSkyLightStrength > 0.001f && !flashlightActive &&
                         g_WorldEnvWork.isFogEnabled && lum > 0.05f && Pc_PlayerCellOutdoor())
                            ? 1
                            : 0;
    {
        /* [SKYLIGHT] state on change (why it is on or off here). */
        static int s_prev = -1;
        const int  key = g_PsyX_SkyLightOn | (flashlightActive << 1) | (Pc_PlayerCellOutdoor() << 2) |
                        ((lum > 0.05f) << 3);

        if (key != s_prev)
        {
            s_prev = key;
            SH_DBG("[SKYLIGHT] on=%d flashlight=%d outdoor=%d lightfog=%d (lum %.2f)", g_PsyX_SkyLightOn,
                   flashlightActive, Pc_PlayerCellOutdoor(), lum > 0.05f, lum);
        }
    }
    if (!g_PsyX_SkyLightOn)
    {
        return;
    }

    /* World Y points down: the light travels +Y, leaning by the tilt toward
     * the azimuth (0 = +Z). */
    tilt = g_PcSkyLightTilt * 0.017453293f;
    az   = g_PcSkyLightAzimuth * 0.017453293f;
    f[0] = sin(tilt) * sin(az);
    f[1] = cos(tilt);
    f[2] = sin(tilt) * cos(az);
    for (r = 0; r < 3; r++)
    {
        g_PsyX_SkyLightDir[r]   = (float)f[r];
        g_PsyX_SkyLightRight[r] = (float)ref[r];
    }
    {
        float fw[3] = { (float)f[0], (float)f[1], (float)f[2] };
        SkyRotate(fw, g_PsyX_SkyLightDirView);
    }

    /* Camera rotation and position in the world (doubles: world coordinates
     * run to hundreds of thousands of Q8 units). */
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
        {
            R[r][c] = (double)GsWSMATRIX.m[r][c] / 4096.0;
        }
    }
    for (c = 0; c < 3; c++)
    {
        cam[c] = -(R[0][c] * GsWSMATRIX.t[0] + R[1][c] * GsWSMATRIX.t[1] + R[2][c] * GsWSMATRIX.t[2]);
    }

    /* Anchor: Harry's feet, or 12 m ahead of the camera when a scripted shot
     * looks at something far from him. */
    P[0] = (double)g_SysWork.playerWork.player.position.vx / 16.0;
    P[1] = (double)g_SysWork.playerWork.player.position.vy / 16.0;
    P[2] = (double)g_SysWork.playerWork.player.position.vz / 16.0;
    if ((P[0] - cam[0]) * (P[0] - cam[0]) + (P[1] - cam[1]) * (P[1] - cam[1]) + (P[2] - cam[2]) * (P[2] - cam[2]) >
        (30.0 * 256.0) * (30.0 * 256.0))
    {
        for (c = 0; c < 3; c++)
        {
            P[c] = cam[c] + R[2][c] * 12.0 * 256.0; /* view +Z (forward) in world = row 2 of R */
        }
    }

    /* Snap the anchor onto the light's texel grid (the same axes the renderer's
     * look-at builds), so every world point keeps its texel while Harry moves. */
    SkyCross(f, ref, s);
    if (s[0] * s[0] + s[1] * s[1] + s[2] * s[2] < 1e-6)
    {
        ref[0] = 0.0;
        ref[2] = 1.0;
        SkyCross(f, ref, s);
        g_PsyX_SkyLightRight[0] = 0.0f;
        g_PsyX_SkyLightRight[2] = 1.0f;
    }
    SkyNormalize(s);
    SkyCross(s, f, u);
    half  = 25.0 * 256.0;
    texel = 2.0 * half / (double)((g_PsyX_ShadowMapSize > 0) ? g_PsyX_ShadowMapSize : 2048);
    ps    = s[0] * P[0] + s[1] * P[1] + s[2] * P[2];
    pu    = u[0] * P[0] + u[1] * P[1] + u[2] * P[2];
    for (c = 0; c < 3; c++)
    {
        P[c] += (floor(ps / texel + 0.5) * texel - ps) * s[c] + (floor(pu / texel + 0.5) * texel - pu) * u[c];
    }

    /* view -> world - anchor: R^T v + (cam - anchor), column-major. */
    for (c = 0; c < 3; c++)
    {
        for (r = 0; r < 3; r++)
        {
            g_PsyX_SkyViewToAnchor[c * 4 + r] = (float)R[c][r];
        }
        g_PsyX_SkyViewToAnchor[c * 4 + 3] = 0.0f;
    }
    for (r = 0; r < 3; r++)
    {
        g_PsyX_SkyViewToAnchor[12 + r] = (float)(cam[r] - P[r]);
    }
    g_PsyX_SkyViewToAnchor[15] = 1.0f;

    g_PsyX_SkyLightParams[0] = g_PcSkyLightStrength * lum;
    g_PsyX_SkyLightParams[1] = g_PcSkyLightSoft;
    g_PsyX_SkyLightParams[2] = (float)half;
    g_PsyX_SkyLightParams[3] = lum;
}

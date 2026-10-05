/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Weather sprites under the PC renderer (weather_style 1 and 2).
 *
 * The snow sim projects each flake with one plain gte_stsxy and offsets the
 * other corners on the CPU, and the rain draws each drop as a one-pixel-wide
 * quad between two projected points, so neither had a precise coordinate (they
 * snapped to the 320x240 grid, ~9 px steps at 2160p), a view position (the
 * per-pixel fog never reached them) or, being semi-transparent, a depth test
 * (they showed through ceilings whose OT bucket sorted behind them). Here the
 * finished vanilla quad keeps its colour and OT placement and gains what the
 * renderer needs: the sub-pixel projection of the same points, view positions
 * for its corners, and an exact depth with a test that never writes.
 *
 * Style 1 keeps the vanilla sprites (the flake's plus of texels, the rain's
 * one-PSX-pixel bar). Style 2 draws them the way they read on a TV instead of
 * as magnified texels: flakes are soft round dots of a real size (a few cm,
 * varying per flake, smaller with distance), rain drops thin soft streaks.
 * The shapes are drawn by the shader (PsyX_SetNextPrimSpriteShape).
 *
 * Called from src/maps/particle.c right before addPrim; map DLLs link to it
 * through the exe's import library. Own state only (no game RNG). */

#include "game.h"
#include "inline_no_dmpsx.h"

#include <math.h>

#include "pc_config.h"

extern void ReadGeomOffset(s32* ofx, s32* ofy); /* math_impl.c: live GTE registers */
extern s32  ReadGeomScreen(void);
extern void PsyX_SetVertexPrecise(void* xyAddr, float sx, float sy, float w);
extern void PsyX_SetVertexViewPos(void* xyAddr, float vx, float vy, float vz);
extern void PsyX_SetNextPrimSoftDepth(void);
extern void PsyX_CancelNextPrimSoftDepth(void);
extern void PsyX_SetNextPrimSpriteShape(int shape);
extern void PsyX_SetNextPrimSzExact(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3);
extern void PsyX_CancelNextPrimSz(void);
extern int  g_PsxUsePgxp;

/* Style 2 sizes, view units (256 = 1 m), and the smallest drawn size in PSX
 * pixels: below it a flake or drop keeps that size and gives up brightness
 * instead, so far ones fade out rather than flicker as sub-pixel specks. */
#define WX_FLAKE_SIZE    13.0f /* ~5 cm, x 0.65..1.5 per flake */
#define WX_FLAKE_MIN_PX  0.55f
#define WX_DROP_WIDTH    2.5f  /* ~1 cm */
#define WX_DROP_MIN_PX   0.6f
#define WX_FLAKE_GAIN    1.35f /* the soft disc covers less than the plus did */
#define WX_DROP_GAIN     2.1f

static int s_wxArmed = 0;

typedef struct
{
    float v[3];  /* view position */
    float sx;    /* precise screen position */
    float sy;
} WxPoint;

typedef struct
{
    float h;
    float ofx;
    float ofy;
} WxProj;

static void WxProjInit(WxProj* pj)
{
    s32 ofx;
    s32 ofy;

    ReadGeomOffset(&ofx, &ofy);
    pj->ofx = (float)ofx;
    pj->ofy = (float)ofy;
    pj->h   = (float)ReadGeomScreen();
    if (pj->h < 1.0f)
    {
        pj->h = 1.0f;
    }
}

/* Through the transform gte_rtps just used. 0 when too close to the eye. */
static int WxProject(const WxProj* pj, const MATRIX* mat, const SVECTOR* posQ8, WxPoint* out)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        out->v[i] = ((float)mat->m[i][0] * posQ8->vx + (float)mat->m[i][1] * posQ8->vy +
                     (float)mat->m[i][2] * posQ8->vz) /
                        4096.0f +
                    (float)mat->t[i];
    }
    if (out->v[2] < 16.0f)
    {
        return 0;
    }
    out->sx = pj->ofx + out->v[0] * pj->h / out->v[2];
    out->sy = pj->ofy + out->v[1] * pj->h / out->v[2];
    return 1;
}

static unsigned short WxSz(float z)
{
    return (unsigned short)((z > 65535.0f) ? 65535.0f : z);
}

static u8 WxScale(u8 c, float k)
{
    const float v = (float)c * k;

    return (u8)((v > 255.0f) ? 255.0f : v);
}

/* Write one corner: integer word first (the precise shadow is keyed on it),
 * then the sub-pixel position and the view position at depth z. */
static void WxCorner(const WxProj* pj, DVECTOR* xy, float sx, float sy, float z)
{
    xy->vx = (s16)floorf(sx + 0.5f);
    xy->vy = (s16)floorf(sy + 0.5f);
    PsyX_SetVertexPrecise(xy, sx, sy, z);
    PsyX_SetVertexViewPos(xy, (sx - pj->ofx) * z / pj->h, (sy - pj->ofy) * z / pj->h, z);
}

static float WxHash(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (float)(x & 0xFFFFu) / 65535.0f;
}

void Pc_WeatherSpritePlace(POLY_FT4* quad, const SVECTOR* posQ8, unsigned seed)
{
    MATRIX  mat;
    WxProj  pj;
    WxPoint p;
    float   w;
    float   hgt;
    float   x0;
    float   y0;
    float   z;

    if (g_PcConfig.weatherStyle <= 0 || quad == NULL || posQ8 == NULL)
    {
        return;
    }

    gte_ReadRotMatrix(&mat);
    WxProjInit(&pj);
    if (!WxProject(&pj, &mat, posQ8, &p))
    {
        return;
    }
    z = p.v[2];

    if (g_PcConfig.weatherStyle >= 2)
    {
        /* A soft round flake centred on the point. */
        float d    = WX_FLAKE_SIZE * (0.65f + 0.85f * WxHash(seed)) * pj.h / z;
        float gain = WX_FLAKE_GAIN;

        /* Without PGXP only the integer corners are drawn: a whole pixel. */
        const float dMin = g_PsxUsePgxp ? WX_FLAKE_MIN_PX : 1.0f;

        if (d < dMin)
        {
            gain *= (d / dMin) * (d / dMin);
            d     = dMin;
        }
        w   = d;
        hgt = d;
        x0  = p.sx - 0.5f * d;
        y0  = p.sy - 0.5f * d;
        quad->r0 = WxScale(quad->r0, gain);
        quad->g0 = WxScale(quad->g0, gain);
        quad->b0 = WxScale(quad->b0, gain);
        PsyX_SetNextPrimSpriteShape(1);
    }
    else
    {
        /* The vanilla sprite, its top-left corner on the point. */
        w   = (float)(quad->x1 - quad->x0);
        hgt = (float)(quad->y2 - quad->y0);
        x0  = p.sx;
        y0  = p.sy;
    }

    /* Corners in vanilla order: 0 top-left, 1 top-right, 2 bottom-left,
     * 3 bottom-right. */
    WxCorner(&pj, (DVECTOR*)&quad->x0, x0, y0, z);
    WxCorner(&pj, (DVECTOR*)&quad->x1, x0 + w, y0, z);
    WxCorner(&pj, (DVECTOR*)&quad->x2, x0, y0 + hgt, z);
    WxCorner(&pj, (DVECTOR*)&quad->x3, x0 + w, y0 + hgt, z);

    /* Exact depth on the linear scale plus a test that never writes. */
    PsyX_SetNextPrimSzExact(WxSz(z), WxSz(z), WxSz(z), WxSz(z));
    PsyX_SetNextPrimSoftDepth();
    s_wxArmed = 1;
}

/* A rain drop: the quad from the tail (corners 0, 2) to the head (1, 3), as
 * vanilla lays it out (u across, v along). Style 1 keeps the one-PSX-pixel
 * bar, style 2 makes it a thin soft streak across the screen direction of
 * the drop, bright at the head and fading toward the tail as the PSX drop
 * texture does (the shader), a little past the head for its soft end. */
void Pc_WeatherStreakPlace(POLY_FT4* quad, const SVECTOR* tailQ8, const SVECTOR* headQ8)
{
    MATRIX  mat;
    WxProj  pj;
    WxPoint t;
    WxPoint hd;
    float   dx;
    float   dy;
    float   len;
    float   nx;
    float   ny;
    float   hw;
    float   ext;
    float   extT;

    if (g_PcConfig.weatherStyle <= 0 || quad == NULL || tailQ8 == NULL || headQ8 == NULL)
    {
        return;
    }

    gte_ReadRotMatrix(&mat);
    WxProjInit(&pj);
    if (!WxProject(&pj, &mat, tailQ8, &t) || !WxProject(&pj, &mat, headQ8, &hd))
    {
        return;
    }

    dx  = hd.sx - t.sx;
    dy  = hd.sy - t.sy;
    len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f)
    {
        dx  = 0.0f;
        dy  = 1.0f;
        len = 0.0f;
    }
    else
    {
        dx /= len;
        dy /= len;
    }

    if (g_PcConfig.weatherStyle >= 2)
    {
        float wpx  = WX_DROP_WIDTH * pj.h / (0.5f * (t.v[2] + hd.v[2]));
        float gain = WX_DROP_GAIN;

        const float wMin = g_PsxUsePgxp ? WX_DROP_MIN_PX : 1.0f;

        if (wpx < wMin)
        {
            gain *= wpx / wMin;
            wpx   = wMin;
        }
        hw   = 0.5f * wpx;
        ext  = 0.1f * len + hw;
        extT = 0.0f;
        quad->r0 = WxScale(quad->r0, gain);
        quad->g0 = WxScale(quad->g0, gain);
        quad->b0 = WxScale(quad->b0, gain);
        PsyX_SetNextPrimSpriteShape(2);
    }
    else
    {
        hw   = 0.5f;
        ext  = 0.0f;
        extT = 0.0f;
    }
    nx = -dy * hw;
    ny = dx * hw;

    WxCorner(&pj, (DVECTOR*)&quad->x0, t.sx - dx * extT - nx, t.sy - dy * extT - ny, t.v[2]);
    WxCorner(&pj, (DVECTOR*)&quad->x2, t.sx - dx * extT + nx, t.sy - dy * extT + ny, t.v[2]);
    WxCorner(&pj, (DVECTOR*)&quad->x1, hd.sx + dx * ext - nx, hd.sy + dy * ext - ny, hd.v[2]);
    WxCorner(&pj, (DVECTOR*)&quad->x3, hd.sx + dx * ext + nx, hd.sy + dy * ext + ny, hd.v[2]);

    PsyX_SetNextPrimSzExact(WxSz(t.v[2]), WxSz(hd.v[2]), WxSz(t.v[2]), WxSz(hd.v[2]));
    PsyX_SetNextPrimSoftDepth();
    s_wxArmed = 1;
}

/* A splash ring on the ground: four world corners in prim order. Keeps its
 * texture; only placement, fog and the depth test. */
void Pc_WeatherQuadPlace(POLY_FT4* quad, const SVECTOR* p0, const SVECTOR* p1, const SVECTOR* p2, const SVECTOR* p3)
{
    MATRIX         mat;
    WxProj         pj;
    WxPoint        p[4];
    const SVECTOR* src[4];
    DVECTOR*       xy[4];
    int            i;

    if (g_PcConfig.weatherStyle <= 0 || quad == NULL)
    {
        return;
    }

    src[0] = p0;
    src[1] = p1;
    src[2] = p2;
    src[3] = p3;
    xy[0]  = (DVECTOR*)&quad->x0;
    xy[1]  = (DVECTOR*)&quad->x1;
    xy[2]  = (DVECTOR*)&quad->x2;
    xy[3]  = (DVECTOR*)&quad->x3;

    gte_ReadRotMatrix(&mat);
    WxProjInit(&pj);
    for (i = 0; i < 4; i++)
    {
        if (!WxProject(&pj, &mat, src[i], &p[i]))
        {
            return;
        }
    }
    for (i = 0; i < 4; i++)
    {
        WxCorner(&pj, xy[i], p[i].sx, p[i].sy, p[i].v[2]);
    }

    PsyX_SetNextPrimSzExact(WxSz(p[0].v[2]), WxSz(p[1].v[2]), WxSz(p[2].v[2]), WxSz(p[3].v[2]));
    PsyX_SetNextPrimSoftDepth();
    s_wxArmed = 1;
}

/* After the addPrim branches: a sprite that was not added must not hand its
 * depth, soft flag or shape on to the next prim. Harmless when addPrim
 * consumed them. */
void Pc_WeatherSpriteDisarm(void)
{
    if (!s_wxArmed)
    {
        return;
    }
    s_wxArmed = 0;
    PsyX_CancelNextPrimSz();
    PsyX_CancelNextPrimSoftDepth();
}

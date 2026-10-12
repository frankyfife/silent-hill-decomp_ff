/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Smoother skeletal animation (config anim_smooth, anim_smooth_npc).
 *
 * Anim_BoneUpdate (bodyprog_anim_800445A4.c) already samples between two
 * keyframes by the fractional animation time, so the pose moves every frame
 * at any frame rate. What still reads as stiff is HOW it blends:
 *
 *  - Piecewise linear: every keyframe is a corner where the velocity jumps,
 *    so limbs move in visible straight segments with a kick at each key.
 *  - The nine entries of the two rotation matrices are mixed one by one. A
 *    mix of two rotations is not a rotation: mid-way the part shrinks a little
 *    and shears, and swings back on the next key.
 *  - The rotations are stored as signed bytes (1/128 steps), so each key is a
 *    slightly skewed, unnormalised matrix; that noise is the fine wobble of
 *    the rigid parts.
 *
 * Here each rotation is made a proper rotation (Gram-Schmidt, then a
 * quaternion) and the pose is a Catmull-Rom spline through the keyframe
 * before, the two around the current time and the one after - inside the
 * animation's own keyframe range, wrapping for loops and clamping at the ends
 * of one-shot animations, so a neighbouring animation stored next to it in
 * the .ANM never leaks in. Every (denoised) keyframe is still hit at its time
 * (timing, events and gameplay are unchanged); only the path between keys
 * changes. Translations take the same spline. The original matrices' size is
 * kept (the bytes top out at 127/128), so the parts do not grow.
 *
 * The keys themselves are denoised first: the byte storage adds rounding noise
 * as large as the motion of a slow animation (Harry's idle), which a spline
 * alone would still trace; see DenoisedQuat.
 *
 * Transitions between two animations (Anim_BlendLinear) additionally ease in
 * and out instead of starting and stopping at full speed.
 *
 * anim_smooth covers Harry, anim_smooth_npc every other character;
 * anim_organic adds follow-through to Harry's locomotion (see below). */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

#include "bodyprog/bodyprog.h"
#include "pc_anim_smooth.h"

float g_PcAnimSmooth    = 1.0f; /* anim_smooth: Harry */
float g_PcAnimSmoothNpc = 1.0f; /* anim_smooth_npc: everybody else */

/* The keyframe range of the animation being sampled, set by the playback
 * functions around their Anim_BoneUpdate call (Pc_AnimSmoothRange). */
static s32 s_rangeStart = -1;
static s32 s_rangeEnd   = -1;
static s32 s_rangeLoop  = 0;

int Pc_AnimSmooth_ParseKey(const char* key, const char* value)
{
    float v = (float)atof(value);

    v = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    if (strcmp(key, "anim_smooth") == 0)
    {
        g_PcAnimSmooth = v;
        return 1;
    }
    if (strcmp(key, "anim_smooth_npc") == 0)
    {
        g_PcAnimSmoothNpc = v;
        return 1;
    }
    if (strcmp(key, "anim_organic") == 0)
    {
        extern float g_PcAnimOrganic;
        float o = (float)atof(value);
        g_PcAnimOrganic = (o < 0.0f) ? 0.0f : ((o > 2.0f) ? 2.0f : o);
        return 1;
    }
    return 0;
}

void Pc_AnimSmoothRange(s32 start, s32 end, s32 loop)
{
    s_rangeStart = start;
    s_rangeEnd   = end;
    s_rangeLoop  = loop;
}

int Pc_AnimSmoothActive(int isPlayer)
{
    return isPlayer ? (g_PcAnimSmooth > 0.5f) : (g_PcAnimSmoothNpc > 0.5f);
}

float Pc_AnimSmoothBlendEase(float t)
{
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

/* ---- small vector / quaternion helpers -------------------------------- */

typedef struct
{
    float w, x, y, z;
} Quat;

static float Dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Signed-byte rotation matrix -> quaternion of its nearest rotation; *scale
 * gets the mean row length (the stored size). Returns 0 for a degenerate or
 * mirrored matrix (caller falls back to the plain blend). */
static int MatToQuat(const s8* m8, Quat* q, float* scale)
{
    float r[3][3];
    float n;
    float c[3];
    int   i;
    int   j;
    float tr;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            r[i][j] = (float)m8[i * 3 + j] / 128.0f;
        }
    }
    *scale = (sqrtf(Dot3(r[0], r[0])) + sqrtf(Dot3(r[1], r[1])) + sqrtf(Dot3(r[2], r[2]))) / 3.0f;

    /* Gram-Schmidt on rows 0, 1; row 2 from their cross product. */
    n = sqrtf(Dot3(r[0], r[0]));
    if (n < 1e-4f)
    {
        return 0;
    }
    for (j = 0; j < 3; j++)
    {
        r[0][j] /= n;
    }
    n = Dot3(r[0], r[1]);
    for (j = 0; j < 3; j++)
    {
        r[1][j] -= n * r[0][j];
    }
    n = sqrtf(Dot3(r[1], r[1]));
    if (n < 1e-4f)
    {
        return 0;
    }
    for (j = 0; j < 3; j++)
    {
        r[1][j] /= n;
    }
    c[0] = r[0][1] * r[1][2] - r[0][2] * r[1][1];
    c[1] = r[0][2] * r[1][0] - r[0][0] * r[1][2];
    c[2] = r[0][0] * r[1][1] - r[0][1] * r[1][0];
    if (Dot3(c, r[2]) < 0.0f)
    {
        return 0; /* mirrored: not a rotation */
    }
    r[2][0] = c[0];
    r[2][1] = c[1];
    r[2][2] = c[2];

    tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0.0f)
    {
        float s = sqrtf(tr + 1.0f) * 2.0f;

        q->w = 0.25f * s;
        q->x = (r[2][1] - r[1][2]) / s;
        q->y = (r[0][2] - r[2][0]) / s;
        q->z = (r[1][0] - r[0][1]) / s;
    }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
    {
        float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;

        q->w = (r[2][1] - r[1][2]) / s;
        q->x = 0.25f * s;
        q->y = (r[0][1] + r[1][0]) / s;
        q->z = (r[0][2] + r[2][0]) / s;
    }
    else if (r[1][1] > r[2][2])
    {
        float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;

        q->w = (r[0][2] - r[2][0]) / s;
        q->x = (r[0][1] + r[1][0]) / s;
        q->y = 0.25f * s;
        q->z = (r[1][2] + r[2][1]) / s;
    }
    else
    {
        float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;

        q->w = (r[1][0] - r[0][1]) / s;
        q->x = (r[0][2] + r[2][0]) / s;
        q->y = (r[1][2] + r[2][1]) / s;
        q->z = 0.25f * s;
    }
    return 1;
}

static void QuatToMatQ12(const Quat* q, float scale, MATRIX* out)
{
    float w = q->w, x = q->x, y = q->y, z = q->z;
    float m[3][3];
    int   i;
    int   j;

    m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    m[0][1] = 2.0f * (x * y - w * z);
    m[0][2] = 2.0f * (x * z + w * y);
    m[1][0] = 2.0f * (x * y + w * z);
    m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    m[1][2] = 2.0f * (y * z - w * x);
    m[2][0] = 2.0f * (x * z - w * y);
    m[2][1] = 2.0f * (y * z + w * x);
    m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            out->m[i][j] = (s16)lrintf(m[i][j] * scale * 4096.0f);
        }
    }
}

static float CatRom(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;

    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

/* Keyframe k moved by d steps inside the current range: wrapping for a loop,
 * held at the ends of a one-shot. Without a known range (or k outside it) the
 * key itself - no neighbours, no filtering. */
static s32 KeyStep(s32 k, s32 d)
{
    s32 n;

    if (s_rangeStart < 0 || s_rangeEnd < s_rangeStart || k < s_rangeStart || k > s_rangeEnd)
    {
        return k;
    }
    n = s_rangeEnd - s_rangeStart + 1;
    k += d;
    if (s_rangeLoop)
    {
        while (k < s_rangeStart)
        {
            k += n;
        }
        while (k > s_rangeEnd)
        {
            k -= n;
        }
        return k;
    }
    return (k < s_rangeStart) ? s_rangeStart : ((k > s_rangeEnd) ? s_rangeEnd : k);
}

static const s8* KeyData(s_AnmHeader* anmHdr, s32 k)
{
    return (const s8*)((u8*)anmHdr + anmHdr->dataOffset) + anmHdr->keyframeDataSize * k;
}

/* ---- keyframe denoise --------------------------------------------------
 *
 * The rotations are signed bytes: one step of one matrix entry is about half
 * a degree, which is as large as a whole key-to-key move of a slow animation.
 * In Harry's idle every joint changes 0.1-0.3 degrees per key and flips
 * direction almost every key (the second difference equals the step): the
 * "breathing" was largely rounding noise, and a spline through noisy keys
 * still wobbles. Each key used here is replaced by an edge-preserving average
 * of itself and its neighbours (time weight x similarity weight): changes
 * within a degree or so - the noise - are averaged out, real motion of
 * several degrees per key (a step, a swing, a hit) is far outside the
 * similarity weight and stays as authored. */

#define DENOISE_R     3     /* keys either side */
#define DENOISE_SIG_T 1.2f  /* time sigma, keys */
#define DENOISE_SIG_A 0.025f /* similarity sigma, radians (~1.4 degrees) */
#define DENOISE_SIG_P 1.5f  /* similarity sigma for translations, in storage steps */

static int KeyQuat(s_AnmHeader* anmHdr, s32 k, s32 ri, Quat* q, float* sc)
{
    return MatToQuat(KeyData(anmHdr, k) + anmHdr->translationBoneCount * 3 + ri * 9, q, sc);
}

static int DenoisedQuat(s_AnmHeader* anmHdr, s32 k, s32 ri, Quat* out, float* scOut)
{
    Quat  c;
    float sc;
    float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float accSc  = 0.0f;
    float wsum   = 0.0f;
    float n;
    s32   d;

    if (!KeyQuat(anmHdr, k, ri, &c, &sc))
    {
        return 0;
    }
    for (d = -DENOISE_R; d <= DENOISE_R; d++)
    {
        s32   kk = KeyStep(k, d);
        Quat  q;
        float s;
        float dot;
        float ang;
        float w;

        if (d != 0 && kk == k)
        {
            continue; /* held end of a one-shot: do not weight the end key twice */
        }
        if (!KeyQuat(anmHdr, kk, ri, &q, &s))
        {
            continue;
        }
        dot = q.w * c.w + q.x * c.x + q.y * c.y + q.z * c.z;
        if (dot < 0.0f)
        {
            dot = -dot;
            q.w = -q.w;
            q.x = -q.x;
            q.y = -q.y;
            q.z = -q.z;
        }
        ang = 2.0f * acosf((dot > 1.0f) ? 1.0f : dot);
        w   = expf(-(float)(d * d) / (2.0f * DENOISE_SIG_T * DENOISE_SIG_T)) *
            expf(-(ang * ang) / (2.0f * DENOISE_SIG_A * DENOISE_SIG_A));
        acc[0] += w * q.w;
        acc[1] += w * q.x;
        acc[2] += w * q.y;
        acc[3] += w * q.z;
        accSc += w * s;
        wsum += w;
    }
    n = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
    if (n < 1e-6f || wsum < 1e-6f)
    {
        *out   = c;
        *scOut = sc;
        return 1;
    }
    out->w = acc[0] / n;
    out->x = acc[1] / n;
    out->y = acc[2] / n;
    out->z = acc[3] / n;
    *scOut = accSc / wsum;
    return 1;
}

static float DenoisedTrans(s_AnmHeader* anmHdr, s32 k, s32 ti, s32 axis)
{
    const float c = (float)KeyData(anmHdr, k)[ti * 3 + axis];
    float       acc = 0.0f;
    float       wsum = 0.0f;
    s32         d;

    for (d = -DENOISE_R; d <= DENOISE_R; d++)
    {
        s32   kk = KeyStep(k, d);
        float v;
        float w;

        if (d != 0 && kk == k)
        {
            continue;
        }
        v = (float)KeyData(anmHdr, kk)[ti * 3 + axis];
        w = expf(-(float)(d * d) / (2.0f * DENOISE_SIG_T * DENOISE_SIG_T)) *
            expf(-((v - c) * (v - c)) / (2.0f * DENOISE_SIG_P * DENOISE_SIG_P));
        acc += w * v;
        wsum += w;
    }
    return (wsum > 1e-6f) ? acc / wsum : c;
}

/* ---- organic locomotion ---------------------------------------------------
 *
 * anim_organic (0..2, default 1): Harry's walk, run, backward walk and side
 * steps were authored as one rigid pose sequence - torso, arms and head all
 * turn exactly in step with the legs, which reads mechanical. Two touches of
 * what a body does on its own:
 *
 *  - Follow-through: the parts further from the hips sample the cycle a
 *    little later (torso a quarter key, out to 1.6 keys at the hands), so the
 *    arms swing after the legs instead of in lock-step with them. Hips and
 *    legs stay exactly on time (foot contacts, footstep sounds).
 *  - A steadier head: it takes back part of the torso's bob and sway around
 *    the cycle's mean pose, the way one keeps one's gaze level while walking.
 *
 * Only inside Harry's own locomotion loops (base keyframes 1-148). */

float g_PcAnimOrganic = 1.0f;

#define ORGANIC_KF_LO 1
#define ORGANIC_KF_HI 148
#define HEAD_STEADY   0.4f

static const float s_organicLag[18] = {
    0.0f,  /* root */
    0.25f, /* torso */
    0.5f,  /* head */
    0.6f,  /* left shoulder */
    0.9f,  /* left upper arm */
    1.3f,  /* left forearm */
    1.6f,  /* left hand */
    0.6f,  /* right shoulder */
    0.9f,  /* right upper arm */
    1.3f,  /* right forearm */
    1.6f,  /* right hand */
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f /* hips, legs */
};

static int OrganicActive(int isPlayer, s_AnmHeader* anmHdr)
{
    return isPlayer && g_PcAnimOrganic > 0.001f && s_rangeLoop && anmHdr->boneCount == 18 &&
           s_rangeStart >= ORGANIC_KF_LO && s_rangeEnd <= ORGANIC_KF_HI && s_rangeEnd > s_rangeStart + 2;
}

static Quat QuatMul(Quat a, Quat b)
{
    Quat r;

    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static Quat QuatConj(Quat a)
{
    a.x = -a.x;
    a.y = -a.y;
    a.z = -a.z;
    return a;
}

/* Identity -> q by s (0..1), small angles: nlerp. */
static Quat QuatScale(Quat q, float s)
{
    Quat  r;
    float n;

    if (q.w < 0.0f)
    {
        q.w = -q.w;
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
    }
    r.w = 1.0f + (q.w - 1.0f) * s;
    r.x = q.x * s;
    r.y = q.y * s;
    r.z = q.z * s;
    n   = sqrtf(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    r.w /= n;
    r.x /= n;
    r.y /= n;
    r.z /= n;
    return r;
}

/* Mean torso orientation over the current loop (cached per range). */
static int MeanTorso(s_AnmHeader* anmHdr, Quat* out)
{
    static s32  s_cStart = -1, s_cEnd = -1;
    static void* s_cHdr = NULL;
    static Quat s_mean;
    static int  s_ok;
    s32         ri = anmHdr->bindPoses[1].rotationDataIdx;

    if (ri < 0)
    {
        return 0;
    }
    if (s_cStart != s_rangeStart || s_cEnd != s_rangeEnd || s_cHdr != anmHdr)
    {
        float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Quat  first;
        float sc;
        s32   k;
        float n;

        s_cStart = s_rangeStart;
        s_cEnd   = s_rangeEnd;
        s_cHdr   = anmHdr;
        s_ok     = KeyQuat(anmHdr, s_rangeStart, ri, &first, &sc);
        for (k = s_rangeStart; s_ok && k <= s_rangeEnd; k++)
        {
            Quat q;

            if (!KeyQuat(anmHdr, k, ri, &q, &sc))
            {
                continue;
            }
            if (q.w * first.w + q.x * first.x + q.y * first.y + q.z * first.z < 0.0f)
            {
                q.w = -q.w;
                q.x = -q.x;
                q.y = -q.y;
                q.z = -q.z;
            }
            acc[0] += q.w;
            acc[1] += q.x;
            acc[2] += q.y;
            acc[3] += q.z;
        }
        n = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
        if (n < 1e-6f)
        {
            s_ok = 0;
        }
        else
        {
            s_mean.w = acc[0] / n;
            s_mean.x = acc[1] / n;
            s_mean.y = acc[2] / n;
            s_mean.z = acc[3] / n;
        }
    }
    *out = s_mean;
    return s_ok;
}

/* ---- the sampler -------------------------------------------------------- */

/* Keys and blend of keyframe0 + alpha moved back by lag keys inside the loop. */
static void LagKeys(s32 keyframe0, float t, float lag, s32* k0, s32* k1, float* tOut)
{
    const float n    = (float)(s_rangeEnd - s_rangeStart + 1);
    float       time = (float)(keyframe0 - s_rangeStart) + t - lag;
    s32         i;

    while (time < 0.0f)
    {
        time += n;
    }
    while (time >= n)
    {
        time -= n;
    }
    i     = (s32)time;
    *tOut = time - (float)i;
    *k0   = s_rangeStart + i;
    *k1   = (*k0 + 1 > s_rangeEnd) ? s_rangeStart : *k0 + 1;
}

/* One bone's rotation at keys k0 -> k1, blend t. Returns 0 if a key is not a
 * clean rotation. */
static int SampleRot(s_AnmHeader* anmHdr, s32 ri, s32 k0, s32 k1, float t, Quat* r, float* scale)
{
    Quat  q[4];
    float sc[4];
    s32   k[4];
    s32   j;
    float n;

    k[1] = k0;
    k[2] = k1;
    k[0] = (k0 == k1) ? k0 : KeyStep(k0, -1);
    k[3] = (k0 == k1) ? k1 : KeyStep(k1, 1);
    for (j = 0; j < 4; j++)
    {
        if (!DenoisedQuat(anmHdr, k[j], ri, &q[j], &sc[j]))
        {
            return 0;
        }
    }
    /* Keep every quaternion on q1's side of the sphere. */
    for (j = 0; j < 4; j++)
    {
        if (j != 1 && (q[j].w * q[1].w + q[j].x * q[1].x + q[j].y * q[1].y + q[j].z * q[1].z) < 0.0f)
        {
            q[j].w = -q[j].w;
            q[j].x = -q[j].x;
            q[j].y = -q[j].y;
            q[j].z = -q[j].z;
        }
    }
    r->w = CatRom(q[0].w, q[1].w, q[2].w, q[3].w, t);
    r->x = CatRom(q[0].x, q[1].x, q[2].x, q[3].x, t);
    r->y = CatRom(q[0].y, q[1].y, q[2].y, q[3].y, t);
    r->z = CatRom(q[0].z, q[1].z, q[2].z, q[3].z, t);
    n    = sqrtf(r->w * r->w + r->x * r->x + r->y * r->y + r->z * r->z);
    if (n < 1e-6f)
    {
        *r = q[1];
        n  = 1.0f;
    }
    r->w /= n;
    r->x /= n;
    r->y /= n;
    r->z /= n;
    *scale = sc[1] + (sc[2] - sc[1]) * t;
    return 1;
}

void Pc_AnimSmoothBoneUpdate(s_AnmHeader* anmHdr, GsCOORDINATE2* boneCoords, u32 activeBoneIdxs, s32 keyframe0,
                             s32 keyframe1, q19_12 alpha, int isPlayer)
{
    const float t = (float)alpha / 4096.0f;
    s32         k[4];
    s32         boneIdx;
    s32         i;
    int         organic;
    Quat        torsoQ;
    int         torsoOk = 0;
    /* Harry's map animations sit past the base header's keyframeCount in the
     * same buffer (Anim_BoneUpdate trusts his indices for that reason). */
    s32         maxKF = isPlayer ? 0 : (s32)anmHdr->keyframeCount;

    if (maxKF > 0 && (s_rangeStart >= maxKF || s_rangeEnd >= maxKF))
    {
        Pc_AnimSmoothRange(-1, -1, 0); /* range past the data: no neighbours */
    }
    organic = OrganicActive(isPlayer, anmHdr) && keyframe0 >= s_rangeStart && keyframe0 <= s_rangeEnd;

    k[1] = keyframe0;
    k[2] = keyframe1;
    k[0] = (keyframe0 == keyframe1) ? keyframe0 : KeyStep(keyframe0, -1);
    k[3] = (keyframe0 == keyframe1) ? keyframe1 : KeyStep(keyframe1, 1);
    for (i = 0; i < 4; i++)
    {
        if (maxKF > 0 && (k[i] < 0 || k[i] >= maxKF))
        {
            k[i] = (i < 2) ? keyframe0 : keyframe1;
        }
    }

    for (boneIdx = 1; boneIdx < anmHdr->boneCount; boneIdx++)
    {
        s_AnmBindPose* bp = &anmHdr->bindPoses[boneIdx];
        GsCOORDINATE2* bc = &boneCoords[boneIdx];
        s32            scaleLog2;
        s32            ti;
        s32            ri;

        if (!(activeBoneIdxs & (1 << boneIdx)))
        {
            continue;
        }
        bc->flg   = false;
        scaleLog2 = anmHdr->scaleLog2;

        ti = bp->translationDataIdx;
        if (ti >= 0)
        {
            for (i = 0; i < 3; i++)
            {
                float p[4];
                s32   j;

                for (j = 0; j < 4; j++)
                {
                    p[j] = DenoisedTrans(anmHdr, k[j], ti, i) * (float)(1 << scaleLog2);
                }
                bc->coord.t[i] = (s32)lrintf(CatRom(p[0], p[1], p[2], p[3], t));
            }
            if (ti == 0)
            {
                bc->coord.t[1] -= anmHdr->rootYOffset;
            }
        }

        ri = bp->rotationDataIdx;
        if (ri >= 0)
        {
            s32   b0 = keyframe0;
            s32   b1 = keyframe1;
            float bt = t;
            Quat  r;
            float scale;

            if (organic && boneIdx < 18 && s_organicLag[boneIdx] > 0.0f)
            {
                LagKeys(keyframe0, t, s_organicLag[boneIdx] * g_PcAnimOrganic, &b0, &b1, &bt);
            }
            if (!SampleRot(anmHdr, ri, b0, b1, bt, &r, &scale))
            {
                /* Not a clean rotation: the retail element-wise blend. */
                const s8* a = KeyData(anmHdr, keyframe0) + anmHdr->translationBoneCount * 3 + ri * 9;
                const s8* b = KeyData(anmHdr, keyframe1) + anmHdr->translationBoneCount * 3 + ri * 9;
                s32       e;

                for (e = 0; e < 9; e++)
                {
                    bc->coord.m[e / 3][e % 3] = (a[e] << 5) + (((b[e] - a[e]) * alpha) >> 7);
                }
                continue;
            }
            if (organic && boneIdx == 1)
            {
                torsoQ  = r;
                torsoOk = 1;
            }
            if (organic && boneIdx == 2 && torsoOk)
            {
                Quat mean;

                if (MeanTorso(anmHdr, &mean))
                {
                    /* torso = mean * dev; the head takes back part of dev. */
                    Quat dev = QuatMul(QuatConj(mean), torsoQ);
                    float s  = HEAD_STEADY * ((g_PcAnimOrganic > 1.0f) ? 1.0f : g_PcAnimOrganic);

                    r = QuatMul(QuatScale(QuatConj(dev), s), r);
                }
            }
            QuatToMatQ12(&r, scale, &bc->coord);
        }
    }
}

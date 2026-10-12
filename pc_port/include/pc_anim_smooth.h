/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ANIM_SMOOTH_H
#define PC_ANIM_SMOOTH_H

/* Smoother skeletal animation, pc_anim_smooth.c. */

extern float g_PcAnimSmooth;    /* anim_smooth: Harry */
extern float g_PcAnimSmoothNpc; /* anim_smooth_npc: every other character */

int   Pc_AnimSmooth_ParseKey(const char* key, const char* value);
int   Pc_AnimSmoothActive(int isPlayer);
/* Keyframe range of the animation the next Anim_BoneUpdate samples
 * (start < 0 = unknown: plain two-key blend). */
void  Pc_AnimSmoothRange(s32 start, s32 end, s32 loop);
float Pc_AnimSmoothBlendEase(float t);
void  Pc_AnimSmoothBoneUpdate(s_AnmHeader* anmHdr, GsCOORDINATE2* boneCoords, u32 activeBoneIdxs, s32 keyframe0,
                              s32 keyframe1, q19_12 alpha, int isPlayer);

#endif

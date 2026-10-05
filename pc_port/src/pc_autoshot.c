/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unattended visual checks, for testing without anyone at the screen:
 *
 *   -exec "<console line>"                 run once the first gameplay frame is
 *                                          reached (repeatable, up to 8)
 *   -autoshot <delay_s> <count> <interval_s> <prefix>
 *                                          from that frame on, save <count>
 *                                          frames as <prefix>_<n>.bmp, then quit
 *   -autowalk <start_s> <duration_s> [mask]
 *                                          hold PSX buttons (default 0x10 = Up)
 *                                          for that window, e.g. to check how
 *                                          effects behave while Harry moves
 *                                          (repeatable, up to 8; masks OR)
 *   -continue                              title menu: Continue = load the
 *                                          newest save (pair with skip_intros 1)
 *
 * Combine with the level select: -map map2_s00 -exec "fogprobe 1"
 * -autoshot 6 3 2 shots/fog. Time is wall clock, so a paused or slow game still
 * captures; the frames come from the presented image (PsyX_render.cpp). */

#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "sh_log.h"

extern char g_PsyX_ScreenshotPath[512];
extern void Pc_ConsoleExec(const char* line);

#define AUTOSHOT_EXEC_MAX 8

static char   s_exec[AUTOSHOT_EXEC_MAX][256];
static int    s_execCount = 0;
static int    s_shotCount = 0;
static float  s_delay     = 0.0f;
static float  s_interval  = 1.0f;
static char   s_prefix[256];
static int    s_started   = 0;
static Uint32 s_startMs   = 0;
static int    s_taken     = 0;
static int    s_quitArmed = 0;
/* Up to 8 -autowalk windows; overlapping ones OR their masks (e.g. a held
 * direction plus short Cross taps to page through a dialogue). */
/* -continue: the title menu picks Continue (the newest save) by itself. */
int g_PcAutoContinue = 0;

#define AUTOWALK_MAX 8
static float    s_walkStart[AUTOWALK_MAX];
static float    s_walkDur[AUTOWALK_MAX];
static unsigned s_walkMask[AUTOWALK_MAX];
static int      s_walkCount = 0;

extern unsigned short g_PsyX_PadForceMask;

int Pc_AutoShot_ParseArg(int argc, char* argv[], int i)
{
    if (strcmp(argv[i], "-exec") == 0 && i + 1 < argc)
    {
        if (s_execCount < AUTOSHOT_EXEC_MAX)
        {
            char* c;

            strncpy(s_exec[s_execCount], argv[i + 1], sizeof(s_exec[0]) - 1);
            s_exec[s_execCount][sizeof(s_exec[0]) - 1] = '\0';
            /* The console types upper case and its commands compare that way. */
            for (c = s_exec[s_execCount]; *c; c++)
            {
                *c = (char)toupper((unsigned char)*c);
            }
            s_execCount++;
        }
        return 2;
    }
    if (strcmp(argv[i], "-continue") == 0)
    {
        g_PcAutoContinue = 1;
        return 1;
    }
    if (strcmp(argv[i], "-autowalk") == 0 && i + 2 < argc)
    {
        int w = (s_walkCount < AUTOWALK_MAX) ? s_walkCount++ : AUTOWALK_MAX - 1;

        s_walkStart[w] = (float)atof(argv[i + 1]);
        s_walkDur[w]   = (float)atof(argv[i + 2]);
        s_walkMask[w]  = 0x10;
        if (i + 3 < argc && argv[i + 3][0] == '0' && (argv[i + 3][1] == 'x' || argv[i + 3][1] == 'X'))
        {
            s_walkMask[w] = (unsigned)strtoul(argv[i + 3], NULL, 16);
            return 4;
        }
        return 3;
    }
    if (strcmp(argv[i], "-autoshot") == 0 && i + 4 < argc)
    {
        s_delay     = (float)atof(argv[i + 1]);
        s_shotCount = atoi(argv[i + 2]);
        s_interval  = (float)atof(argv[i + 3]);
        strncpy(s_prefix, argv[i + 4], sizeof(s_prefix) - 1);
        s_prefix[sizeof(s_prefix) - 1] = '\0';
        return 5;
    }
    return 0;
}

/* Once per main-loop iteration, after the game update. */
void Pc_AutoShot_Frame(void)
{
    float t;

    if (s_execCount == 0 && s_shotCount <= 0 && s_walkCount == 0)
    {
        return;
    }

    if (!s_started)
    {
        int i;

        if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
        {
            return;
        }
        s_started = 1;
        s_startMs = SDL_GetTicks();
        for (i = 0; i < s_execCount; i++)
        {
            SH_LOG("Autoshot: exec '%s'", s_exec[i]);
            Pc_ConsoleExec(s_exec[i]);
        }
    }

    if (s_walkCount > 0)
    {
        float    tw   = (SDL_GetTicks() - s_startMs) / 1000.0f;
        unsigned mask = 0;
        int      w;

        for (w = 0; w < s_walkCount; w++)
        {
            if (tw >= s_walkStart[w] && tw < s_walkStart[w] + s_walkDur[w])
            {
                mask |= s_walkMask[w];
            }
        }
        g_PsyX_PadForceMask = (unsigned short)mask;
    }

    if (s_shotCount <= 0)
    {
        return;
    }

    /* The last request is written at the end of the frame that made it; quit
     * on the iteration after, once nothing is pending. */
    if (s_quitArmed)
    {
        if (g_PsyX_ScreenshotPath[0] == '\0')
        {
            SDL_Event quit;
            memset(&quit, 0, sizeof(quit));
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
            s_shotCount = 0;
        }
        return;
    }

    t = (SDL_GetTicks() - s_startMs) / 1000.0f;
    if (g_PsyX_ScreenshotPath[0] == '\0' && t >= s_delay + s_interval * s_taken)
    {
        snprintf(g_PsyX_ScreenshotPath, 512, "%s_%d.bmp", s_prefix, s_taken);
        SH_LOG("Autoshot: %s at %.1fs", g_PsyX_ScreenshotPath, t);
        s_taken++;
        if (s_taken >= s_shotCount)
        {
            s_quitArmed = 1;
        }
    }
}

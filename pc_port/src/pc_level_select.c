/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Level select from the command line (the launcher's Level / Spawn /
 * Difficulty pickers):
 *
 *   -map <id>                     start a New Game straight in that map
 *   -spawn default|door:<n>|room:<n>
 *   -difficulty easy|normal|hard
 *
 * Nothing here is written to config.cfg: a test launch must not turn every
 * later boot into that test. It rides the existing New-Game-to-map path
 * (title.c: skip-to-game, GameBoot_SavegameInitialize, starter loadout), and
 * only swaps the point Harry is placed on. Spawn points come from the tables
 * the randomizer harvests (pc_rando_data.h): door arrivals authored in other
 * maps, and the map's own room-arrival points. */

#include "game.h"
#include "bodyprog/map/map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_config.h"
#include "sh_log.h"

extern int                 MapRegistry_FindByName(const char* name);
extern int                 Pc_Rando_ArrivalCount(int mapIdx);
extern const s_MapPoint2d* Pc_Rando_ArrivalGet(int mapIdx, int i);
extern int                 Pc_Rando_RoomCount(int mapIdx);
extern const s_MapPoint2d* Pc_Rando_RoomGet(int mapIdx, int i);

enum { SPAWN_DEFAULT, SPAWN_DOOR, SPAWN_ROOM };

static char s_map[32];
static int  s_spawnKind  = SPAWN_DEFAULT;
static int  s_spawnIdx   = 0;
static int  s_difficulty = -1; /* title.c difficulty index: 0 easy, 1 normal, 2 hard */

/* Returns how many argv entries it consumed (0 = not ours). */
int Pc_LevelSelect_ParseArg(int argc, char* argv[], int i)
{
    if (strcmp(argv[i], "-map") == 0 && i + 1 < argc)
    {
        strncpy(s_map, argv[i + 1], sizeof(s_map) - 1);
        s_map[sizeof(s_map) - 1] = '\0';
        return 2;
    }
    if (strcmp(argv[i], "-spawn") == 0 && i + 1 < argc)
    {
        const char* v = argv[i + 1];

        if (strncmp(v, "door:", 5) == 0)
        {
            s_spawnKind = SPAWN_DOOR;
            s_spawnIdx  = atoi(v + 5);
        }
        else if (strncmp(v, "room:", 5) == 0)
        {
            s_spawnKind = SPAWN_ROOM;
            s_spawnIdx  = atoi(v + 5);
        }
        else
        {
            s_spawnKind = SPAWN_DEFAULT;
        }
        return 2;
    }
    if (strcmp(argv[i], "-difficulty") == 0 && i + 1 < argc)
    {
        const char* v = argv[i + 1];

        s_difficulty = (strcmp(v, "easy") == 0 || strcmp(v, "0") == 0) ? 0 :
                       (strcmp(v, "hard") == 0 || strcmp(v, "2") == 0) ? 2 : 1;
        return 2;
    }
    return 0;
}

/* After PcConfig_Load and before Pc_Rando_Init: the command line wins over the
 * config for this session only. */
void Pc_LevelSelect_Apply(void)
{
    if (s_map[0] == '\0')
    {
        return;
    }
    if (MapRegistry_FindByName(s_map) < 0)
    {
        SH_LOG("Level select: unknown map '%s', ignored", s_map);
        s_map[0] = '\0';
        return;
    }

    strncpy(g_PcConfig.mapName, s_map, sizeof(g_PcConfig.mapName) - 1);
    g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
    g_PcConfig.skipIntros = 2;
    /* The randomizer forces its own start map; a level-select launch is a
     * plain game in the chosen map. */
    g_PcConfig.randomizer = 0;

    SH_LOG("Level select: %s, spawn %s:%d, difficulty %d", s_map,
           s_spawnKind == SPAWN_DOOR ? "door" : (s_spawnKind == SPAWN_ROOM ? "room" : "default"),
           s_spawnIdx, s_difficulty);
}

int Pc_LevelSelect_DifficultyIdx(int fallback)
{
    return (s_map[0] != '\0' && s_difficulty >= 0) ? s_difficulty : fallback;
}

/* The point the New Game places Harry on. Falls back to the map's own
 * mapPoints[0] for the default, a stale index, or a map change since boot. */
const s_MapPoint2d* Pc_LevelSelect_SpawnPoint(const s_MapPoint2d* fallback)
{
    const s_MapPoint2d* p = NULL;
    int                 mapIdx;

    if (s_map[0] == '\0' || s_spawnKind == SPAWN_DEFAULT ||
        strcmp(g_PcConfig.mapName, s_map) != 0)
    {
        return fallback;
    }

    mapIdx = MapRegistry_FindByName(s_map);
    if (s_spawnKind == SPAWN_DOOR)
    {
        p = Pc_Rando_ArrivalGet(mapIdx, s_spawnIdx);
    }
    else
    {
        p = Pc_Rando_RoomGet(mapIdx, s_spawnIdx);
    }

    if (p == NULL)
    {
        SH_LOG("Level select: no %s spawn %d in %s, using the map start",
               s_spawnKind == SPAWN_DOOR ? "door" : "room", s_spawnIdx, s_map);
        return fallback;
    }
    return p;
}

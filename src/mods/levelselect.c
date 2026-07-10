#include "global.h"
#include "fox_map.h"
#include "mods.h"
#include "port/hooks/Events.h"

extern PlanetId sPlanetList[15];
extern PlanetId sCurrentPlanetId;
extern u8 sLevelStartState;
extern s32 D_menu_801CD968;
extern s32 sMapState;
extern s32 sMapSubState;

static bool sLevelSelectorActive = false;

void Map_LevelStart_AudioSpecSetup(LevelId level);
void Map_CurrentLevel_Setup(void);
void Map_PositionCursor(void);
void Map_PlayLevel(void);

static PlanetId sPlanetArray[][3] = {
    { PLANET_CORNERIA, PLANET_CORNERIA, PLANET_CORNERIA }, { PLANET_METEO, PLANET_METEO, PLANET_SECTOR_Y },
    { PLANET_FORTUNA, PLANET_KATINA, PLANET_AQUAS },       { PLANET_SECTOR_X, PLANET_SOLAR, PLANET_ZONESS },
    { PLANET_TITANIA, PLANET_MACBETH, PLANET_SECTOR_Z },   { PLANET_BOLSE, PLANET_BOLSE, PLANET_AREA_6 },
    { PLANET_VENOM, PLANET_VENOM, SAVE_SLOT_VENOM_2 },
};

typedef struct {
    PlanetId planetId;
    const char* spokenName;
    char* displayText;
    s32 displayX;
} LevelSelectStartOption;

// Single source of truth for the alternate-start set: the spoken name (event payload) and the
// drawn text/position come from the same entry so speech cannot desync from the screen.
static const LevelSelectStartOption sStartOptions[] = {
    { PLANET_METEO, "Warp Zone", "WARP ZONE", 80 + 60 + 10 },
    { PLANET_SECTOR_X, "Warp Zone", "WARP ZONE", 80 + 60 + 10 },
    { PLANET_VENOM, "Andross", "ANDROSS", 80 + 60 + 3 },
    { PLANET_AREA_6, "Beta SB", "BETA SB", 80 + 60 - 3 },
};

static const LevelSelectStartOption* Map_LevelSelect_StartOption(PlanetId planetId, s32 startOption) {
    s32 i;

    if (!startOption) {
        return NULL;
    }
    for (i = 0; i < ARRAY_COUNT(sStartOptions); i++) {
        if (sStartOptions[i].planetId == planetId) {
            return &sStartOptions[i];
        }
    }
    return NULL;
}

void Map_LevelSelect_Reset(void) {
    sLevelSelectorActive = false;
}

void Map_LevelSelect(void) {
    static s32 mission = 0;
    static s32 difficulty = 0;
    static char* sLevelSelectPlanetNames[] = {
        "METEO",  "AREA 6",   "BOLSE",   "SECTOR Z", "SECTOR X", "SECTOR Y", "KATINA", "MACBETH",
        "ZONESS", "CORNERIA", "TITANIA", "AQUAS",    "FORTUNA",  "VENOM 1",  "SOLAR",  "VENOM 2",
    };
    static s32 startOption = 0;
    static s32 timer = 30;
    static s32 startLevel = 0;
    static s32 announcedSelection = -1;

    // static f32 zStart = 0.0f;
    // f32 zInc;
    s32 nextPlanetId;
    OSContPad* contPress = &gControllerPress[gMainController];

    if ((sMapState != MAP_IDLE) && (sMapState != MAP_ZOOM_PLANET)) {
        sLevelSelectorActive = false;
        return;
    }

    if (contPress->button & L_JPAD) {
        mission--;
        if (mission < 0) {
            mission = 6;
        }
    } else if (contPress->button & R_JPAD) {
        mission++;
        if (mission > 6) {
            mission = 0;
        }
    } else if ((contPress->button & U_JPAD) && (mission != 0)) {
        difficulty++;
        if (difficulty > 2) {
            difficulty = 0;
        }
        if ((difficulty == 1) && ((mission == 1) || (mission == 5) || (mission == 6))) {
            difficulty = 2;
        }
    } else if ((contPress->button & D_JPAD) && (mission != 0)) {
        difficulty--;
        if ((difficulty != 2) && ((mission == 1) || (mission == 5) || (mission == 6))) {
            difficulty--;
        }
        if (difficulty < 0) {
            difficulty = 2;
        }
    }

    const s32 rawSelection = sPlanetArray[mission][difficulty];
    nextPlanetId = (rawSelection == SAVE_SLOT_VENOM_2) ? PLANET_VENOM : rawSelection;
    if (sCurrentPlanetId != nextPlanetId) {
        sCurrentPlanetId = nextPlanetId;
        startOption = 0;
        Map_CurrentLevel_Setup();
        Map_PositionCursor();
    }
    const LevelSelectStartOption* startOptionInfo = Map_LevelSelect_StartOption(sCurrentPlanetId, startOption);
    if (!sLevelSelectorActive) {
        sLevelSelectorActive = true;
        announcedSelection = rawSelection;
        CALL_EVENT(LevelSelectorReadyEvent, sLevelSelectPlanetNames[rawSelection],
                   startOptionInfo != NULL ? startOptionInfo->spokenName : NULL);
    } else if (rawSelection != announcedSelection) {
        // Venom 1 and Venom 2 share sCurrentPlanetId, so track the raw selection (which the
        // displayed name is indexed by) rather than the planet change.
        announcedSelection = rawSelection;
        CALL_EVENT(LevelSelectorSelectionChangedEvent, sLevelSelectPlanetNames[rawSelection],
                   startOptionInfo != NULL ? startOptionInfo->spokenName : NULL);
    }
    if (contPress->button & L_TRIG) {
        const LevelSelectStartOption* previousStartOptionInfo = startOptionInfo;
        startOption ^= 1;
        startOptionInfo = Map_LevelSelect_StartOption(sCurrentPlanetId, startOption);
        if (previousStartOptionInfo != startOptionInfo) {
            CALL_EVENT(LevelSelectorStartOptionChangedEvent, sLevelSelectPlanetNames[rawSelection],
                       startOptionInfo != NULL ? startOptionInfo->spokenName : NULL);
        }
    }

    int y = 225;

    /* Draw */
    if ((sCurrentPlanetId >= 0) && (sCurrentPlanetId < PLANET_MAX)) {
        RCP_SetupDL(&gMasterDisp, SETUPDL_83_OPTIONAL);
        gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 0, 255);

        Graphics_DisplaySmallText(20, y, 1.0f, 1.0f, "PLANET:");
        Graphics_DisplaySmallText(80, y, 1.0f, 1.0f, sLevelSelectPlanetNames[sPlanetArray[mission][difficulty]]);

        if (startOptionInfo != NULL) {
            Graphics_DisplaySmallText(startOptionInfo->displayX, y, 1.0f, 1.0f, startOptionInfo->displayText);
        }
    }

    if (gControllerPress[0].button & A_BUTTON) {
        timer = 15;
        startLevel = 1;
    }

    if (timer > 0) {
        timer--;
    }

    // Bypass briefing
    if ((CVarGetInteger("gSkipBriefing", 0) == 1) || (sCurrentPlanetId == PLANET_VENOM)) {
        if ((timer == 0) && (startLevel == 1)) {
            if ((sMapState == 2) && (sMapSubState > 0)) {
                if (sCurrentPlanetId == PLANET_VENOM) {
                    if (startOption) {
                        gCurrentLevel = LEVEL_VENOM_ANDROSS;
                    } else if (sPlanetArray[mission][difficulty] == SAVE_SLOT_VENOM_2) {
                        gCurrentLevel = LEVEL_VENOM_2;
                    }
                } else if ((sCurrentPlanetId == PLANET_AREA_6) && startOption) {
                    gCurrentLevel = LEVEL_UNK_4;
                }
                Map_LevelStart_AudioSpecSetup(gCurrentLevel);
                sLevelStartState = 0;
                D_menu_801CD968 = 0;
                Map_PlayLevel();
                if (startOption && ((gCurrentLevel == LEVEL_METEO) || (gCurrentLevel == LEVEL_SECTOR_X) ||
                                    (sPlanetArray[mission][difficulty] == SAVE_SLOT_VENOM_2))) {
                    gLevelPhase = 1;
                }
            }
        }
    }
}

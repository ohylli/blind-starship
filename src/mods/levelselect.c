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

static const char* Map_LevelSelect_StartOptionName(PlanetId planetId, s32 startOption) {
    if (!startOption) {
        return NULL;
    }
    if ((planetId == PLANET_SECTOR_X) || (planetId == PLANET_METEO)) {
        return "Warp Zone";
    }
    if (planetId == PLANET_VENOM) {
        return "Andross";
    }
    if (planetId == PLANET_AREA_6) {
        return "Beta SB";
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

    nextPlanetId =
        (sPlanetArray[mission][difficulty] == SAVE_SLOT_VENOM_2) ? PLANET_VENOM : sPlanetArray[mission][difficulty];
    const bool selectionChanged = sCurrentPlanetId != nextPlanetId;
    if (selectionChanged) {
        sCurrentPlanetId = nextPlanetId;
        startOption = 0;
        Map_CurrentLevel_Setup();
        Map_PositionCursor();
    }
    const char* startOptionName = Map_LevelSelect_StartOptionName(sCurrentPlanetId, startOption);
    if (!sLevelSelectorActive) {
        sLevelSelectorActive = true;
        CALL_EVENT(LevelSelectorReadyEvent, sLevelSelectPlanetNames[sPlanetArray[mission][difficulty]], startOptionName);
    } else if (selectionChanged) {
        CALL_EVENT(LevelSelectorSelectionChangedEvent, sLevelSelectPlanetNames[sPlanetArray[mission][difficulty]],
                   startOptionName);
    }
    if (contPress->button & L_TRIG) {
        const char* previousStartOptionName = startOptionName;
        startOption ^= 1;
        startOptionName = Map_LevelSelect_StartOptionName(sCurrentPlanetId, startOption);
        if (previousStartOptionName != startOptionName) {
            CALL_EVENT(LevelSelectorStartOptionChangedEvent, sLevelSelectPlanetNames[sPlanetArray[mission][difficulty]],
                       startOptionName);
        }
    }

    int y = 225;

    /* Draw */
    if ((sCurrentPlanetId >= 0) && (sCurrentPlanetId < PLANET_MAX)) {
        RCP_SetupDL(&gMasterDisp, SETUPDL_83_OPTIONAL);
        gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 0, 255);

        Graphics_DisplaySmallText(20, y, 1.0f, 1.0f, "PLANET:");
        Graphics_DisplaySmallText(80, y, 1.0f, 1.0f, sLevelSelectPlanetNames[sPlanetArray[mission][difficulty]]);

        if (startOptionName != NULL) {
            if ((sCurrentPlanetId == PLANET_SECTOR_X) || (sCurrentPlanetId == PLANET_METEO)) {
                Graphics_DisplaySmallText(80 + 60 + 10, y, 1.0f, 1.0f, "WARP ZONE");
            } else if (sCurrentPlanetId == PLANET_VENOM) {
                Graphics_DisplaySmallText(80 + 60 + 3, y, 1.0f, 1.0f, "ANDROSS");
            } else if (sCurrentPlanetId == PLANET_AREA_6) {
                Graphics_DisplaySmallText(80 + 60 - 3, y, 1.0f, 1.0f, "BETA SB");
            }
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

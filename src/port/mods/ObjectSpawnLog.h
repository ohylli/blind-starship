#pragma once

#ifdef __cplusplus
void ObjectSpawnLog_Init();

// Level id -> "LEVEL_CORNERIA" style name; shared with the debug server's dumps.
const char* Starship_LevelName(int level);
#endif

// ObjectId_GetName and EventId_GetName are defined in ObjectIdNames.generated.c
// and EventIdNames.generated.c (still C); keep C linkage so the unmangled
// definitions still resolve from this C++ TU.
#ifdef __cplusplus
extern "C" {
#endif
const char* ObjectId_GetName(int id);
const char* EventId_GetName(int id);
#ifdef __cplusplus
}
#endif

#pragma once

// Coordinator for the Star Fox gameplay cues. The cues themselves live one-per-file in
// accessibility_cues/ (RingCue, EnemyCue, AimCue) over the shared helpers there
// (CueCommon, CueScan); this entry point only registers them and owns the registry
// housekeeping. Adding a cue: new file pair in accessibility_cues/ following any
// existing one, plus one Register call in AccessibilityCues_Init.

void AccessibilityCues_Init();
void AccessibilityCues_Exit();

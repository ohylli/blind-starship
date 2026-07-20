#pragma once

void Accessibility_Init();
void Accessibility_Exit();

bool Accessibility_IsScreenReaderEnabled();

// True only while the player is actually flying the ship, i.e. outside the
// cutscenes and menu/map states that take control away. Shared by the audio-cue
// mod and the level-mode announcement; see the definition for the per-clause
// rationale.
bool Accessibility_PlayerHasControl();

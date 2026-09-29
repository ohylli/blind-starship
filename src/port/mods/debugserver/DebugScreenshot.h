#pragma once

// The debug server's `screenshot` command: reads a freshly drawn game frame back from the
// renderer and answers with it as JSON (dimensions + base64 RGB pixels) for the client to
// save as an image (tools/debug_client.py screenshot <file.png>). Socket-only — the pixels
// have nowhere to go from the in-game ImGui console.
//
// DebugScreenshot_Register is called from DebugCommands_Init. DebugScreenshot_FrameTick
// drives an in-flight capture and must run once per main-loop tick after the frame was
// drawn and before DebugServer_FrameTick (GameEngine::StartFrame). DebugScreenshot_Exit
// undoes a capture's temporary setting if the game quits mid-capture; it must run before
// the exit-time config save (GameEngine::Destroy).

void DebugScreenshot_Register();
void DebugScreenshot_FrameTick();
void DebugScreenshot_Exit();

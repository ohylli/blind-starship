# Blind Starship

## About

Blind Starship is a fork of the original [Starship
project](https://github.com/HarbourMasters/Starship). It aims to explore making
the game accessible to blind players via screen reader announcements, audio
cues and possible other game modifications. The project is in very early
development so how feasible most of this is is anybody's guess.

## Current status

There is a early proof-of-concept [alpha
release](https://github.com/ohylli/blind-starship/releases/tag/alpha) that is
kept up to date with the latest publicly released code. It has a screen reader
integration via [PRISM](https://github.com/ethindp/prism). Currently used to
make the game's main menu, sounds options and pause menu accessible.

For gameplay there is partial accessibility for the game's training mode. In
training you practice maneuvering you ship by flying through rings and fighting
some enemies. These rings and enemies have their own sound cues. By default they
are rendered as real 3D binaural (HRTF) audio via [Steam
Audio](https://valvesoftware.github.io/steam-audio/), which gives true left /
right and front / back positioning through headphones. There is also an older
fallback that drives the cues through the game's own sound engine, where stereo
pan indicates left / right; you can switch between the two (see) the configuration
section). In both, pitch indicates above / below you — high pitch is above you,
lower below. The cue tracks the closest target. Note: controls are typical
fligth controls back / down on a stick makes you go up.

There are also some spoken screen reader announcements during gameplay. In
training the number of rings you have flown through in a row (your streak) is
spoken as you pass each ring, and you are told when a streak is broken by a
missed ring. Across all levels your score is announced by speaking the bonus
that pops up on screen when you destroy an enemy, matching what a sighted player
sees: the hits you scored ("hit +3") plus your new total, or "great" and "extra
life" for the special bonuses. Both of these are per event announcements that can
get chatty, so they share a single toggle that is on by default (see the
configuration section if you want to turn them off). Changing the camera view
also has spoken announcements.

Training has 2 phases. The first is on rails flying containing only rings,
enemies and some item pick ups. So no obstacles. Second phase  is in all-range
mode (you can freely fly around a small arena) with some enemies and obstacles
with no accessibility features to help you. Note the enemy audio cue for now
does not work here. And in case you are familiar with the game and are thinking "wait
that is not what I remember." you are absolutely right. Currently the first
phase of training is simplified all obstacles are removed (see
the configuration section how to bring them back). When more features are added
like obstacle audio cues, these simplifications will be removed. The enemy audio
cue works also in the main game though might not cover all enemies and does not
include bosses.

## Road map

In no particular order planned features or things to investigate include:

- Refine audio cues for enemies.
- Audio cues for obstacles.
- Add an audio cue glossary with both general sound effects and blind starship
  audio cues.
- Making more game menus and screens accessible.
- Adding more screen reader announcements to gameplay: health, remaining lives,
  number of bombs etc.
- Making the ports own configuration UI accessed via F1 accessible.
- Possibly simplifying some levels if there simply is too much stuff going
  on that cannot be communicated via audio.
- Adding audio description to cut scenes.
- Including a Mac version to the release (already confirmed that Blind Starship
  works on Mac.)
- Verifying that Linux build works (needs a Linux tester)

## Discord

You can discuss Blind Starship and provide feedback on the game's channel on the Accessibility
Disco server: https://discord.gg/QZMsMT2WsZ

# Quick Start

Starship does not include any copyrighted assets.  You are required to provide a supported copy of the game.

### 1. Verify your ROM dump
The supported ROMs are US 1.0 and US 1.1 Rev A versions. You can verify you have dumped a supported copy of the game by using the SHA-1 File Checksum Online at https://www.romhacking.net/hash/. 

* The SHA-1 hash for a US 1.0 ROM is D8B1088520F7C5F81433292A9258C1184AFA1457.
* The SHA-1 hash for a US 1.1 ROM is 09F0D105F476B00EFA5303A3EBC42E60A7753B7A.

Starship also supports voice language replacement use from both EU (Lylat) and JP (Japanese) when used in conjunction with an US ROM.

Note: JP and EU versions of the game are not supported for the base asset O2R creation, a US ROM must be used for it, and you can only use one voice language replacement at a time (Either EU or JP).

### 2. Verify your ROM is in .z64 format
Your ROM needs to be in .z64 format. If it's in .n64 format, use the following to convert it to a .z64: https://hack64.net/tools/swapper.php

### 3. Download the Blind Starship [alpha release](https://github.com/ohylli/blind-starship/releases/tag/alpha)

### 4. Generating the OTR from the ROM and Play!
#### Windows
* Extract every file from the zip into a folder of your choosing.
* Run starship.exe and select your US 1.0 or US 1.1 ROM.

#### Linux (not currently supported)
* Extract every file from the zip into a folder of your choosing.
* Execute starship.appimage. You may have to chmod +x the appimage via terminal.

#### MacOS (not currently supported)
* Extract every file from the zip into a folder of your choosing.
* Run starship and select your US 1.0 or US 1.1 ROM.

# Configuration

### Default controls

Starship can be played with a keyboard, an Xbox-style gamepad, or any other
controller SDL recognises. The table below lists each in-game action, its
default key/button on keyboard and Xbox controllers, and the original Nintendo
64 control it stands in for. 

| Action | Keyboard | Xbox controller | N64 controller |
| - | - | - | - |
| Steer the Arwing, move in menus | W / A / S / D | Left stick | Control Stick |
| Fire blaster (hold to charge a lock-on shot), confirm menu selection | X | A | A |
| Fire a bomb, back in menus | C | X | B |
| Boost | Left arrow | Y, or right trigger | C-left |
| Brake | Down arrow | B, or left trigger | C-down |
| Tilt left for a sharper turn | Z | Left bumper (LB) | Z trigger |
| Tilt right for a sharper turn | R | Right bumper (RB) | R trigger |
| Barrel roll — deflects incoming enemy fire | double-tap Z or R | double-tap LB or RB | double-tap Z or R |
| Somersault — flip backwards | Left arrow + S | (Y or right trigger) + left stick down | C-left + Control Stick down |
| U-turn — reverse direction (all-range mode only) | Down arrow + S | (B or left trigger) + left stick down | C-down + Control Stick down |
| Switch camera view | Up arrow | Right stick up | C-up |
| Respond to an incoming radio message | Right arrow | Right stick right | C-right |
| Pause / confirm a menu selection | Space | Start (menu) | Start |
| Menu cursor (no Arwing function) | T / F / G / H | D-pad | D-Pad |

### Other shortcuts
| Keys | Action |
| - | - |
| F1 | Toggle menubar (not yet accessibile) |
| F4 | Reset |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |

### Simplified training mode

As mentioned in current status by default the training mode is simplified. if
you want the original training back you can do it by editing the game's settings
file `starship.cfg.json` created after first launch to the same folder where
you have the game. Change the value of `AccessibilityTrainingMinimal` from 1 to
0 i.e. change 1 to 0 on the line tthat looks like:

```
"gAccessibilityTrainingMinimal": 1,
```

### Gameplay announcements

The spoken score and training ring streak announcements are on by default. If
you find them too chatty you can turn them both off by editing the game's
settings file `starship.cfg.json` created after first launch to the same folder
where you have the game. Change the value of `gAccessibilityScoreAnnounce` from 1
to 0 i.e. change 1 to 0 on the line that looks like:

```
"gAccessibilityScoreAnnounce": 1,
```

### 3D audio cues

By default the ring and enemy audio cues are rendered as real 3D binaural (HRTF)
audio via [Steam Audio](https://valvesoftware.github.io/steam-audio/), giving
true left / right and front / back positioning through headphones. If you prefer
the older cues driven by the game's own sound engine (stereo pan for left /
right, pitch for above / below), you can switch back by editing the game's
settings file `starship.cfg.json` created after first launch to the same folder
where you have the game. Change the value of `gAccessibilityCue3D` from 1 to 0
i.e. change 1 to 0 on the line that looks like:

```
"gAccessibilityCue3D": 1,
```

### Graphics Backends
Currently, there are three rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), and Metal (macOS). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `starship.cfg.json` file by finding the line `"Backend":{`... and changing the `id` value to `3` and set the `Name` to `OpenGL`. `DirectX 11` with id `2` is the default on Windows. `Metal` with id `4` is the default on macOS.

# Custom Assets
Custom assets are packed in `.o2r` or `.otr` files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.o2r`/`.otr` files, check out the following tools:
* [**retro - OTR and O2R generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin (Note that SF64 is not supported at this time)**](https://github.com/HarbourMasters/fast64)

# Development
### Building

If you want to manually compile Blind Starship, please consult the [building instructions](https://github.com/ohylli/blind-starship/blob/main/docs/BUILDING.md).

# Blind Starship

## About

Blind Starship is a fork of the original [Starship
project](https://github.com/HarbourMasters/Starship). It aims to explore making
the game accessible to blind players via screen reader announcements, audio
cues and possible other game modifications. The project is in early
development so how feasible most of this is is anybody's guess.

## Current status

There is a early proof-of-concept [alpha
release](https://github.com/ohylli/blind-starship/releases/tag/alpha) that is
kept up to date with the latest publicly released code. It has a screen reader
integration via [PRISM](https://github.com/ethindp/prism). Currently used to
make the game's main menu, sounds options and pause menu accessible. The port's
own settings menu accessed via F1 is also accessible.

For gameplay there is partial accessibility for the game's training mode. In
training you practice maneuvering you ship by flying through rings and fighting
some enemies. These rings and enemies have their own sound cues. They are
rendered as real 3D binaural (HRTF) audio via [Steam
Audio](https://valvesoftware.github.io/steam-audio/), which gives true left /
right and front / back positioning through headphones.  IN addition pitch
indicates above / below you — high pitch is above you, lower below. Note:
controls are typical fligth controls back / down on a stick makes you go up The
ring cue tracks the closest ring and the enemy cue the 5 closest ones. The
number of tracked enemies is configurable (see the configuration section).

There is also an aiming audio cue serving two purposes: telling your location on
the screen and telling you when your aim is close to an enemy. Stereo pan
indicates left / right on the screen and pitch the height. The cue gets faster
when your aim nears an enemy. You can preview all the cue sounds and change
their volumes  (see configuration section).

There are also some spoken screen reader announcements during gameplay. The game
mode: on-rails or all-range, is announced in the start of a level and when it
changes in the middle of a level. In training the number of rings you have flown
through in a row (your streak) is spoken as you pass each ring, and you are told
when a streak is broken by a missed ring. Across all levels your score is
announced by speaking the bonus that pops up on screen when you destroy an
enemy, matching what a sighted player sees: the hits you scored ("hit +3") plus
your new total, or "great" and "extra life" for the special bonuses. Both of
these are per event announcements that can get chatty, so they share a single
toggle that is on by default (see the configuration section if you want to turn
them off). Changing the camera view also has spoken announcements. Instructional
radio messages in training that are not voiced are announced by screen reader.
This can be toggled off in settings (see configuration section).

Training has 2 phases. The first is on rails flying containing only rings,
enemies and some item pick ups. So no obstacles. Second phase  is in all-range
mode (you can freely fly around a small arena) with some enemies and no
obstacles. And in case you are familiar with the game and are thinking "wait
that is not what I remember." you are absolutely right. Currently training is
simplified all obstacles are removed (see the configuration section how to bring
them back). When more features are added like obstacle audio cues, these
simplifications will be removed. The enemy and aiming audio cues work also in the main game
though might not cover all enemies and do not include bosses.

## Road map

In no particular order planned features or things to investigate include:

- Refine audio cues for enemies e.g. include bosses.
- Audio cues for obstacles.
- Add an audio cue glossary with both general sound effects and blind starship
  audio cues.
- Making more game menus and screens accessible.
- Adding more screen reader announcements to gameplay: health, remaining lives,
  number of bombs etc.
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
| F1 | Toggle menubar (note freezes all game controls so pause first during gameplay.) |
| F4 | Reset |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |

### Screen reader

The spoken menu, screen, and gameplay announcements are on by default and are
what make the game playable without sight. If for some reason you want to turn
them off you can do it from the settings menu (f1 -> settings -> blind starship
-> screen reader). Or by editing the game's settings file `starship.cfg.json`
created after first launch to the same folder where you have the game. Change
the value of `gAccessibilityScreenReader` from 1 to 0 i.e. change 1 to 0 on the
line that looks like:

```
"gAccessibilityScreenReader": 1,
```

### Simplified training mode

As mentioned in current status by default the training mode is simplified. if
you want the original training back you can do it from the settings menu (f1 ->
settings -> blind starship -> minimal training). Or by editing the game's settings
file `starship.cfg.json` created after first launch to the same folder where
you have the game. Change the value of `AccessibilityTrainingMinimal` from 1 to
0 i.e. change 1 to 0 on the line tthat looks like:

```
"gAccessibilityTrainingMinimal": 1,
```

### Gameplay announcements

The spoken score and training ring streak announcements are on by default. If
you find them too chatty you can turn them both off from the settings menu (f1
-> settings -> blind starship -> score announcements). Or by editing the game's
settings file `starship.cfg.json` created after first launch to the same folder
where you have the game. Change the value of `gAccessibilityScoreAnnounce` from 1
to 0 i.e. change 1 to 0 on the line that looks like:

```
"gAccessibilityScoreAnnounce": 1,
```

### Radio messages

Some of training's instructional radio messages are shown on screen as text
only, without any voice acting. These are read out by the screen reader so you
do not miss them. They repeat on a timer as the training script loops, so if you
already know them and find them repetitive you can turn just these off (the rest
of the screen reader stays on). Do it from the settings menu (f1 -> settings ->
blind starship -> radio text). Or by editing the game's settings file
`starship.cfg.json` created after first launch to the same folder where you have
the game. Change the value of `gAccessibilityRadioText` from 1 to 0 i.e. change 1
to 0 on the line that looks like:

```
"gAccessibilityRadioText": 1,
```

### 3D audio cues

The ring and enemy audio cues are rendered as real 3D binaural (HRTF) audio via
[Steam Audio](https://valvesoftware.github.io/steam-audio/), giving true left /
right and front / back positioning through headphones. Wear headphones for the
positioning to work.

You can adjust how loud the cues are from the settings menu (f1 -> settings ->
blind starship -> cue volumes). There you get an "all cues" slider that sets the
overall cue loudness, a per-cue slider for each cue (ring guide, enemy locator),
and a preview button beside each one that plays a short sample of that cue
straight ahead so you can set the level by ear. The cues are also scaled by the
game's own master volume, so turning the game down turns the cues down too.

The volumes are saved in `starship.cfg.json` (created after first launch in the
folder where you have the game) as `gAccessibilityCueMasterVolume` for the "all
cues" slider and `gAccessibilityCueVolume.Ring` /
`gAccessibilityCueVolume.Enemy` / `gAccessibilityCueVolume.Aim` for the per-cue
sliders, if you would rather edit them there.

### Number of enemy locator cues

The enemy locator cue sounds at the closest lockable enemies. By default it
tracks several of them at once, but you can change how many. Fewer voices makes
the soundscape less busy; a single voice gives you just the nearest enemy. You
can set this from the settings menu (f1 -> settings -> blind starship -> enemy
locator voices). Or by editing the game's settings file `starship.cfg.json`
created after first launch to the same folder where you have the game. Change
the value of `gAccessibilityEnemyCueVoices` to the number of enemies you want
tracked, on the line that looks like:

```
"gAccessibilityEnemyCueVoices": 5,
```

### Aim guide cue

The aim guide is the aiming audio cue described in current status: a repeating
click whose stereo pan tells you where you are aiming left / right on the
screen, whose pitch tells you the height, and that clicks faster as your aim
nears an enemy. It only sounds while you are flying the Arwing. Because a
continuous click is more tiring to listen to than the other cues, it has its own
toggle so you can turn it off without losing the ring and enemy cues. It is on by
default. You can toggle it from the settings menu (f1 -> settings -> blind
starship -> aim guide). Or by editing the game's settings file
`starship.cfg.json` created after first launch to the same folder where you have
the game. Change the value of `gAccessibilityAimCue` from 1 to 0 i.e. change 1 to
0 on the line that looks like:

```
"gAccessibilityAimCue": 1,
```

### Developer options for audio cues

Beyond the everyday settings above, the developer menu (f1 -> developer -> blind
starship) holds a handful of Blind Starship specific tools aimed at testing and
tuning the audio cues rather than day to day play. You do not need any of them to
play the game, but if you are curious or want to help tune the cues by ear they
are worth knowing about. Among them are a "Cue3D test bench" that plays a cue on
demand so you can hear it in isolation and try out different positions, pitches
and effects, and a set of sliders and toggles for how the cues sound — for
example how a cue's pitch is produced and how sounds behind you are muffled to
help tell front from back. These are live controls: changes take effect
immediately so you can compare options while listening. They are experimental
and meant for tuning, so feel free to explore and set them back to their
defaults if something sounds off.

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

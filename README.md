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

There are also obstacle audio cues. They warn you about solid things
you cannot shoot down, such as buildings, rock walls, hills, mountains and
large ships. They work in on-rails levels and all-range battles. There are
four of them:

- Obstacle warning: a low buzz from straight ahead that beats faster the
  closer you get to an obstacle on your course. On rails "on your course"
  means you would hit it if you kept your current position. In all-range it
  means the direction you are flying.
- Obstacle beside: a pulsing mid-pitched chord that tells you something is on
  your left or right that you would hit if you steered that way. Stereo pan
  tells the side, and how far it is panned tells the distance: the center is
  you, so the closer the obstacle, the nearer the center the chord is, and an
  obstacle further out is panned fully to its side.
- Obstacle above: a high chord that sounds while something above you would
  block you if you climbed. It gets louder the closer the obstacle is.
- Obstacle below: a low chord that works the same way for something below you,
  such as a hill you are flying over.

An obstacle the buzz is already warning you about is not repeated by the
beside, above and below chords. Unlike the ring and enemy cues, the obstacle
cues are not 3D audio: the beside chord uses plain stereo pan and the others
come from straight ahead.

Please note that the obstacle cues are very much a work in progress. They do
not claim to give you all the awareness you need to avoid every obstacle, and
some things are not covered at all. For example the ground, water and lava are
not warned about, nor are bosses. Shapes are also approximated, so a warning
can come a bit early or late, or for a near miss. Feedback on how they work
for you is very welcome (see the Discord section).

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
that is not what I remember." you are absolutely right. By default training is
simplified: all obstacles are removed. This lets you get used to flying and
the other cues without having to dodge obstacles at the same time. That also
means the obstacle cues stay silent in training. Once you feel comfortable you
can bring the obstacles back and try the obstacle cues on them (see the
configuration section). The enemy, aiming and obstacle audio cues work also in
the main game, though the enemy cue might not cover all enemies and does not
include bosses.

A good way to get used to the obstacle cues is at the very beginning of
training, with minimal training turned off (f1 -> settings -> blind starship ->
minimal training). There are some buildings on the left and right edge. If you
fly in the center you will not hear anything since they are too far away for
the cues to pick up. If you move a little left or right you hear the obstacle
beside cue from them. If you move fully to the right or left you will start
hearing the obstacle warning buzz. You would not actually hit anything, since
the buildings are not quite tall enough, but the obstacle warning also warns
about near misses. You will also briefly hear the obstacle below cue when you
fly over a building, but that is soon replaced by the obstacle warning buzz
for the next building. If you fly a bit higher you will only hear the obstacle
below cue. And if you fly a bit lower you will start hitting the buildings and
can hear how the obstacle warning buzz behaves then. You
can quit training from the pause menu and return to it from the main menu so you
can easily experiment with the obstacle cues.

## Road map

In no particular order planned features or things to investigate include:

- Refine audio cues for enemies e.g. include bosses.
- Refine the obstacle audio cues based on feedback e.g. cover more kinds of
  obstacles and give better awareness of the space around you.
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
you have the game. Change the value of `gAccessibilityTrainingMinimal` from 1 to
0 i.e. change 1 to 0 on the line that looks like:

```
"gAccessibilityTrainingMinimal": 1,
```

With minimal training on there are no obstacles in training, so the obstacle
cues stay silent there. Turn it off if you want to practice with the obstacle
cues in training.

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
positioning to work. The aim guide and obstacle cues use plain stereo instead,
but headphones help with them too.

You can adjust how loud the cues are from the settings menu (f1 -> settings ->
blind starship -> cue volumes). There you get an "all cues" slider that sets the
overall cue loudness, a per-cue slider for each cue (ring guide, enemy locator,
aim guide, obstacle warning, obstacle beside, obstacle above, obstacle below),
and a preview button beside each one that plays a short sample of that cue
so you can set the level by ear. The cues are also scaled by the
game's own master volume, so turning the game down turns the cues down too.

The volumes are saved in `starship.cfg.json` (created after first launch in the
folder where you have the game) as `gAccessibilityCueMasterVolume` for the "all
cues" slider and `gAccessibilityCueVolume.Ring` /
`gAccessibilityCueVolume.Enemy` / `gAccessibilityCueVolume.Aim` /
`gAccessibilityCueVolume.ObstacleAhead` / `gAccessibilityCueVolume.ObstacleSide`
/ `gAccessibilityCueVolume.ObstacleAbove` /
`gAccessibilityCueVolume.ObstacleBelow` for the per-cue sliders, if you would
rather edit them there.

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

### Obstacle cues

The obstacle cues (obstacle warning, beside, above and below, described in
current status) share one toggle that is on by default. You can turn them all
off without losing the other cues from the settings menu (f1 -> settings ->
blind starship -> obstacle warning). Or by editing the game's settings file
`starship.cfg.json` created after first launch to the same folder where you
have the game. Change the value of `gAccessibilityObstacleCue` from 1 to 0 i.e.
change 1 to 0 on the line that looks like:

```
"gAccessibilityObstacleCue": 1,
```

Remember that with minimal training on (the default) there are no obstacles in
training, so to hear these cues there either turn minimal training off or play
the main game.

### Developer options for audio cues

Beyond the everyday settings above, the developer menu (f1 -> developer -> blind
starship) holds a handful of Blind Starship specific tools aimed at testing and
tuning the audio cues rather than day to day play. You do not need any of them to
play the game, but if you are curious or want to help tune the cues by ear they
are worth knowing about. Among them are a "Cue3D test bench" that plays a cue on
demand so you can hear it in isolation and try out different positions, pitches
and effects, and a set of sliders and toggles for how the cues sound — for
example how a cue's pitch is produced and how sounds behind you are muffled to
help tell front from back. There are also sliders for the obstacle cues, such as
how far ahead an obstacle starts to sound, how fast the warning buzz beats and
how close an obstacle must be to count as beside, above or below you. These are live controls: changes take effect
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

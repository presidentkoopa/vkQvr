# 🥽 vkQvr

**Quake, in VR, without giving anything up.**

There's really only one serious VR Quake port, and using it means leaving a lot on the table. [quakevr](https://github.com/vittorioromeo/quakevr) nails the part that matters — you holster the shotgun on your hip, yank ammo across the room with an open hand, swing the axe like you mean it, headbutt a zombie. Nobody else has come close to that. But it rides on a dated GL renderer, and it welded its VR data into the engine's fixed entity layout, which quietly bumped a checksum every third-party mod is compared against. The price is steep: Arcane Dimensions, Copper, Alkaline, and basically everything else on Quaddicted simply refuses to load. Meanwhile [vkQuake](https://github.com/Novum/vkQuake) is the modern engine everyone actually runs — Vulkan, multithreaded, plays every mod ever made — and it has no idea what a hand is.

This is the transplant. quakevr's VR layer onto vkQuake's engine, via OpenXR, with one hard rule: **the checksum stays at 5927.** Every VR field is looked up by name at load time and degrades to nothing when a mod doesn't declare it, so AD still boots — you just get the engine-side half (stereo, head and hand tracking, a weapon that sits properly in your fist, room-scale) instead of the full holster-and-grab layer, which needs the VR game code. Built for one person on a Quest 2 over Virtual Desktop, so it's pragmatic where it needs to be and obsessive where it counts.

### How far along?

**~75%.** The foundation is done and not coming back up: OpenXR session, per-eye stereo at native headset resolution, room-scale, controllers, hands, torso, holsters you can actually reach into, and — as of the last big push — a QuakeC bridge that genuinely *runs*, after fifteen VR functions turned out to be silently executing unrelated engine code (`WriteVec3` was calling `max()`, which is why firing a shotgun killed the connection every single time). What's left is mostly visible rather than structural: the in-headset options menu is still unported, some hand and holster placement wants tuning by eye, and multiplayer is completely untested — deliberately last in line. Call it 75% of a VR port and 0% of a co-op one.

---

The sections below are inherited from upstream vkQuake and still apply.

## Installation

Windows and Linux binaries can be found in [Releases](https://github.com/Novum/vkQuake/releases).
MacOS (both Apple Silicon and 64-bit Intel) binaries are at [Mac Source Ports](https://www.macsourceports.com/game/quake).

You will need the Quake game data. Copy `id1/` from a Quake install (Steam, GOG, or the CD) next to the executable.
`openxr_loader.dll` must sit beside `vkQuake.exe` for VR to initialise.

## Vulkan

A Vulkan 1.1 capable GPU and current drivers are required. Dynamic shadows and dynamic lights additionally
require ray tracing support.

## Building (Windows)

Install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) — the build needs `glslangValidator` and `spirv-opt`
from it, and the installer requires admin rights. Then build the solution:

```
MSBuild Windows\VisualStudio\vkquake.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output lands in `Windows\VisualStudio\Build-vkQuake\x64\Release\`.

For Linux and macOS build instructions, see [upstream vkQuake](https://github.com/Novum/vkQuake#building).

## Running in VR

```
vkQuake.exe -basedir <path to Quake> -game vrqc -vr +map start
```

`-game vrqc` supplies the VR game code and the hand, body and holster models. Without it you still get stereo
rendering and tracking, but none of the QuakeC-side features. `-vrprogs` forces the VR game code without a
headset, which is useful for debugging that layer on the desktop.

## Credits

id Software, [QuakeSpasm](http://quakespasm.sourceforge.net/), [QuakeSpasm-Spiked](https://triptohell.info/moodles/qss/),
[vkQuake](https://github.com/Novum/vkQuake) by Axel Gneiting, and [quakevr](https://github.com/vittorioromeo/quakevr)
by Vittorio Romeo — whose VR design work this port is, in the most literal sense, copying.

GPLv2, same as everything upstream. See `LICENSE.txt`.

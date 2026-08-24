# 🥽 vkQvr

**Quake, in VR, without giving anything up.**

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

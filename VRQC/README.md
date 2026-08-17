# VRQC — quakevr's QuakeC, adapted to load in this engine

This is vittorioromeo's quakevr QuakeC (https://github.com/vittorioromeo/quakevr),
which is where quakevr's actual gameplay lives: holsters, weapon throwing,
force-grab, dual-wielding, per-weapon clips, hand-touch item pickup.

## What was changed, and why

quakevr declares its VR entity fields and several extra globals *inside* the
system blocks, before `end_sys_fields` / `end_sys_globals`. That folds them into
the progdefs checksum and shifts the layout the engine expects, which is exactly
why quakevr's engine reports `PROGHEADER_CRC 52440` and cannot load any other
mod's `progs.dat`.

Four edits to `defs.qc` move them out:

1. `#include "vr_sys_fields.qc"` moved below `end_sys_fields`.
2. `.float ammocounter`, `.float lastwatertime`, `.vector v_viewangle` moved
   below `end_sys_fields`.
3. `spawnServerFromSaveFile` and `parm17`..`parm40` moved below
   `end_sys_globals`.
4. `OnSpawnServerBeforeLoad`, `OnSpawnServerAfterLoad`, `OnLoadGame` moved below
   `end_sys_globals` — they are quakevr engine callbacks this engine does not
   invoke.

Nothing is deleted; every field and global still exists and the QC uses them
normally. The engine picks the VR fields up by name through its QCEXTFIELD
table (see `Quake/progs.h`), so they work without being in the system block.

Result: **CRC 5927**, stock global and field layout, verified name-for-name
against `Quake/progdefs.q1`. Vanilla, Arcane Dimensions and this VR progs.dat
all load in the same engine.

## Known gaps

- `parm17`..`parm40` no longer persist across level changes, because the engine
  only carries `parm1`..`parm16`. Holster contents will not survive a map
  transition until spawnparm handling is extended.
- `OnSpawnServer*` / `OnLoadGame` are never called.

## Building

    fteqcc64.exe -O3 -Fautoproto -progdefs -Olo -Fiffloat -Fifvector \
                 -Fvectorlogic -Flo -Fsubscope -Wno-F209 -Wno-F208

Outputs `vrprogs.dat`; install as `<basedir>/vrqc/progs.dat` and run with
`-game vrqc`.

# quakevr → vkQuake port: state and handover

Source of truth is `E:\quakevr`, and **both halves of it matter**: the C++ under
`Quake/`, and `ReleaseFiles/Id1/config.cfg`, which is the author's shipped,
tuned configuration. Most of the real numbers live in the config, not the code.
The code defaults are nothing like what quakevr actually ships.

## Measuring coverage

Do not trust prose about coverage, including this file's. Run:

    sh tools/portcheck.sh

It counts from both source trees every time, so it cannot drift. Earlier
versions of this document claimed "50 of 76 done" while grab, reload and
off-hand attack did not exist at all.

Current output: cvars OK, builtins OK, QC fields OK, commands 17 missing (all
vkQuake-vs-QuakeSpasm engine differences — `fitztest`, `gl_info`, `hunk_print` —
plus `voip`), stats 46 missing.

The 46 stats are quakevr's transport for holster contents. They are deliberately
**not** ported: the same data is read straight off the server edict through
`QCEXTFIELD`, which works on a listen server and avoids a protocol change that
would break ordinary clients. Revisit only if dedicated-server VR is wanted.

## What is done

Foundation: OpenXR session, per-eye stereo at the headset's native resolution,
asymmetric projection, room-scale, the full QuakeC bridge at **stock CRC 5927**.

Parity: 178/178 global cvars, 32/32 builtins, 2112 per-weapon cvars
(`vr_wofs_<field>_<nn>`, 66 fields × 32 slots) so quakevr's config binds
directly and any weapon is tunable live.

Body and weapons: hand on the controller, weapon seated so its grip lands in the
hand, uniform hand scaling, holsters drawn with their contents, weapon-mounted
buttons, two-handed aiming with virtual stock, weapon weight, flick reload.

Input: grab, reload, flick reload, off-hand attack, `+button3`–`8`. Controller
inputs are OR-ed with the bound commands, so both paths work.

## The lesson that cost the most time

**quakevr's numbers come in matched sets.** Its per-part offsets compensate for
its own non-uniform model scaling and its own anchor vertices. Importing any one
without the machinery it corrects for makes things worse, every time — the palm
five units off, the fingers five units back, the weapon two units sideways.

Where this port scales uniformly and lets models place themselves, those offsets
are zeroed on purpose. Do not "restore" them to quakevr's values without also
restoring quakevr's scaling rule.

## Numbers that are ours, not quakevr's

Everything else is lifted. These four are not, and each is commented in place:

- `vr_gunangle -30` — measured on this hardware. quakevr's 39.5 is against
  OpenVR's controller pose; this uses OpenXR's aim pose.
- `vr_vrtorso_z_offset -51` — quakevr's -45 suits its author's 1.646 m
  calibration; the `head_z_mult` term scales with height.
- `vr_fingers_and_base_x 2` — a forward nudge, dialled by eye.
- `vr_wpn_autoscale 26` — derived from what quakevr's own table produces for
  `v_shot` (40.1 units → 26.7).

## Mod compatibility

The split that governs everything: **engine-side VR works with any progs.dat;
QC-side VR needs the VR progs.**

Engine-side, and therefore always available: stereo rendering, head tracking,
hands drawn and tracked, weapon held in the hand and auto-scaled, room-scale
movement, snap/smooth turn, weapon aiming and firing along the controller.

QC-side, and therefore only with `vrqc`: storing weapons in holsters, force
grab, melee damage, headbutting, positional damage, hand-touch pickup, throwing.

So a mod with its own `progs.dat` — Arcane Dimensions, Team Fortress, most
large mods — will run and be playable in VR, with the weapon in your hand at a
sane size, but without the holster/grab layer. CRC 5927 is what makes them load
at all; do not let it drift.

Arcane Dimensions specifically: its weapon set is recognised by name in the AD
branch of the per-weapon cvars, so its weapons get real numbers rather than
autoscale. Its own progs replaces the VR QC.

Team Fortress: expected to load, but it is QuakeWorld-oriented and
multiplayer-first, so treat it as untested. Nothing about it is known to be
incompatible.

## Adding a weapon

Nothing is required. An unrecognised `progs/v_*.mdl` is auto-scaled to
`vr_wpn_autoscale` on its longest axis and seated by a mesh search for its grip.

To tune one, find its slot with `vr_scaledump` and set the cvars live:

    vr_wofs_scale_02 0.6      // size
    vr_wofs_z_02 12           // along the model's own axes
    vr_wofs_id_07 progs/v_mymod.mdl   // claim an unused slot for a mod weapon

Slots are numbered from 01. `vr_grip_vertex` pins a specific grip vertex if the
mesh search picks badly.

## Multiplayer — untested, design notes only

Nothing here has been run against a second player. What is known from the code:

VR state is client-side. Poses, hands and the drawn body never leave the client,
so a VR player joining a desktop server should work, with the VR player getting
the engine-side features above.

The QC-side features need the **server** running the VR progs. A desktop player
on a VR server is fine — they simply never set the VR fields.

The holster data is read from the server edict directly rather than sent as
stats. That is correct on a listen server and **wrong for a dedicated server**,
where the client has no edict to read. Porting the 46 stats is the fix, and is
the one place the shortcut has a real cost.

## Suggested next work

1. **Polish/QoL**: crosshair is off by default (`vr_crosshair 3` for the faded
   line); no virtual keyboard, so the console needs a real one; `vr_hud_scale`
   is registered but unwired — the panel uses `vr_menu_scale`.
2. **`VK_KHR_multiview`** — one pass instead of two, roughly halves GPU cost.
   Worth more now that rendering is at native eye resolution.
3. **World text renderer** — the five builtins store their data but draw
   nothing; needs a textured world-space pipeline.
4. **The 46 stats**, if dedicated-server or true multiplayer VR is wanted.

## Open bugs, reported but not diagnosed

Found in testing on 2026-08-18 and left unresolved. Start here.

1. **No monsters spawn** in `-game vrqc` on e1m1. Not a missing-QC problem:
   all 32 monster files are listed in `VRQC/progs.src` and present on disk,
   and `vrqc/progs.dat` is current. Undiagnosed. Check whether the QC's
   monster spawn functions are being reached at all, and whether any recent
   engine change (PF_setspawnparms writing parms 17-40 via ED_FindGlobal, or
   the worldtext builtins) is disturbing progs execution.

2. **Holster controls do not respond.** Hotspot detection and the drawn
   holsters both work, so the geometry is fine; what is untested is whether
   the hotspot reaches the QuakeC. Check that the `hotspot` field is written
   to the player edict each frame, alongside `vrbits0`.

3. **No VR options menu.** Not a bug — never ported. quakevr's `menu.cpp`
   carries a large VR menu (torso, holsters, fingers, weapon offsets,
   comfort). Everything is reachable by console cvar, but there is no
   in-headset UI and no virtual keyboard, so the console needs a real
   keyboard. This is the largest single piece of remaining work.

4. **Brightness.** quakevr ships `gamma 0.5, contrast 1`; this was left at
   vkQuake's `0.9 / 1.4`. Set from the console, or change the defaults.

Untested and unverified: weapon-mounted buttons, holstered weapons drawing,
off-hand attack, manual reload, and holsters surviving a level change. All
four were written but never seen running.

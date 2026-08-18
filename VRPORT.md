# quakevr → vkQuake port status

Source of truth: `E:\quakevr` (vittorioromeo/quakevr). Every value and formula is
ported from it, never derived — except where noted below, where quakevr has no
answer to give.

**Cvar parity: 178 of 178.** Verified by diffing every `DEFINE_*CVAR*` in
quakevr's `vr_cvars.cpp` against every `cvar_t` here.

**Builtin parity: 27 of 32.** All quakevr builtins implemented except the five
`worldtext_*`.

Legend: **[x]** done · **[~]** partial · **[ ]** not started

---

## A. Engine foundation

- [x] OpenXR instance, system, session, state machine
- [x] Vulkan via `XR_KHR_vulkan_enable2` on the headset's GPU
- [x] Room-scale STAGE reference space (LOCAL fallback), plus a VIEW space for head velocity
- [x] Per-eye swapchains, compositor frame pacing, true stereo
- [x] Asymmetric per-eye projection, widened cull frustum
- [x] Multithreaded-safe — all `xr*` on the main thread
- [x] UNORM swapchain, neutral gamma/contrast
- [ ] `VK_KHR_multiview` — one pass instead of two, roughly halves GPU cost
- [ ] Offscreen render target at full eye resolution (currently window-sized)

## B. Tracking and view

- [x] Head pose → view, no `STAT_VIEWHEIGHT`, no bob
- [x] `{-z,-x,y}` conversion, `meters_to_units` = `world_scale/(1.5*0.0254)`
- [x] `vr_floor_offset` −16, applied once
- [x] Room-scale movement — head XY discarded from the camera; cannot drift
- [x] Room-scale jump, gated on rise speed and calibrated standing height
- [x] Snap and smooth turn, deadzone, `vr_enable_joystick_turn`
- [ ] `VR_GetBodyYawAngle` — blended head/hand body yaw
- [ ] `VR_GetCrouchRatio` / crouch-aware anchors
- [ ] `VR_PushYaw` on `svc_setview` and teleport

## C. Controllers and hands

- [x] Action set; Touch, Index and generic profiles
- [x] Grip + aim poses, trigger, grip, sticks, buttons, velocity
- [x] Throw velocity averaging, and both throw algorithms
- [x] Haptics, `#81` builtin, `vr_disablehaptics`
- [x] Finger tracking via `XR_EXT_hand_tracking`, falling back to grip
- [x] Hand assembly — `hand_base.mdl` plus five fingers, 27 offset cvars
- [x] Off hand mirrored on Y (own no-cull pipeline; Vulkan bakes winding in)
- [x] Hand scale — quakevr has none, derived from its own sizing rule
- [x] Off-hand angle offsets
- [ ] Weapon-mounted buttons (`VR_DoWpnButton`)

## D. Weapon

- [x] Viewmodel at the hand — position from grip, orientation from aim
- [x] Per-weapon offset and scale table, id1/hipnotic/rogue + Arcane Dimensions
- [x] `VR_ApplyModelMod` + `VR_GetScaleCorrect`, applied to body models too
- [x] Frame correction (`vr_gunangle` −30, `vr_gunyaw`, `vr_gunroll`) on models *and* shot
- [x] Model-only pitch (`vr_gunmodelpitch`)
- [x] `original_scale` snapshot for MDL, MD5, MD3
- [x] Hand grips the weapon — anchored to the mesh, not the controller
- [x] Gun wall collisions
- [x] Weapon weight simulation, position and direction
- [x] Two-handed aiming with virtual stock, eased transitions
- [x] Flick reload
- [ ] Ironsights

## E. Gameplay (QuakeC bridge)

- [x] quakevr's full QC compiles and loads — **CRC 5927**, mods still work
- [x] 68 VR fields via `QCEXTFIELD`, 27 builtins at their exact numbers
- [x] Hand-touch pickup, holster hotspots, `vrbits0`, teleport
- [x] Force grab, melee, headbutt, drops, positional damage, holster/reload/cycle modes
- [x] `vr_enabled` driven from real session state
- [ ] Holsters persisting across level change (needs spawnparm extension)
- [ ] `VR_OnSpawnServer` / `VR_OnClientClearState` lifecycle hooks

## F. Models and presentation

- [x] quakevr asset paks, torso, leg holsters, hands, fingers
- [x] VR HUD on a world-space panel, with all six placement offsets
- [x] World-space crosshair — point, line and faded-line modes
- [x] Holster, hand-axis and velocity debug visualisations
- [~] VR menu — on the HUD panel; virtual keyboard not ported
- [ ] World text (`worldtext_*` builtins, five of them)

## G. Comfort and misc

- [x] Rotating autosave, twelve slots
- [ ] `vr_autosave_on_changelevel` — registered, needs a hook in the level change
- [ ] `vr_hud_scale` — registered but not wired; the panel uses `vr_menu_scale`,
      which is what has actually been verified in the headset
- [ ] `vr_player_shadows`, `vr_msaa`, `vr_movement_mode`, `vr_aimmode`,
      `vr_menumode` — registered, behaviour not implemented
- [ ] Flatscreen mode (`vr_fakevr`) — deliberately deferred

---

## Derived rather than ported

Two numbers here are not quakevr's, because quakevr has none to give. Both are
derived from its own rules rather than picked to look right:

- **`vr_hand_scale` 0.38.** quakevr never scales `hand_base.mdl`; it registers
  only `hand.mdl` at scale 0 to hide the weapon placeholder. Its weapon table
  brings models to true size at 26.25 units/metre — `v_shot.mdl` is 40.1 units
  and `0.5 × 1.333` takes it to 1.02 m, a real shotgun. The same rule on a
  13.1-unit hand model wants about 5 units.
- **The grip anchor.** `HandAnchorVertex` defaults to 0 for every weapon and no
  call overrides it, and vertex 0 is not a grip in either engine — quakevr
  indexes post-dedup VBO order, this indexes raw MDL order. quakevr's real values
  were dialled through its runtime menu and live in a config. So the grip is found
  geometrically: the rearmost fifth of the model by length, lower half, averaged.

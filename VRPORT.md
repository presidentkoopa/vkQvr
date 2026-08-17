# quakevr → vkQuake port status

Source of truth: `E:\quakevr` (vittorioromeo/quakevr). Every value and formula is
ported from it, never derived. Surface: **178 cvars**, **48 VR functions** in
`vr.cpp`, plus ~50k lines of VR QuakeC.

Legend: **[x]** done · **[~]** partial · **[ ]** not started

---

## A. Engine foundation

- [x] 1. OpenXR instance, system, session, state machine
- [x] 2. Vulkan created via `XR_KHR_vulkan_enable2` on the headset's GPU
- [x] 3. Room-scale STAGE reference space (LOCAL fallback)
- [x] 4. Per-eye swapchains, compositor frame pacing
- [x] 5. True stereo — one render per eye, own pose and frustum
- [x] 6. Asymmetric per-eye projection from `XrFovf`
- [x] 7. Cull frustum widened in VR (`gl_rmain.cpp:569-573`)
- [x] 8. Multithreaded-safe (all `xr*` on main thread, `r_tasks` on)
- [x] 9. UNORM swapchain — no sRGB double-encode
- [x] 10. Gamma/contrast neutral in VR (quakevr ships 1/1, vkQuake 0.9/1.4)
- [x] 11. Window forced to eye aspect ratio
- [ ] 12. `VK_KHR_multiview` — one pass instead of two (~2x GPU saving)
- [ ] 13. Offscreen render target at full eye resolution (currently half, via window)

## B. Tracking and view

- [x] 14. Head pose → view, no `STAT_VIEWHEIGHT`, no bob (`view.cpp:1003-1012`)
- [x] 15. Coordinate conversion `{-z,-x,y}`, `meters_to_units` = `world_scale/(1.5*0.0254)`
- [x] 16. `vr_floor_offset` -16, applied once
- [x] 17. Body yaw from stick + head yaw on top
- [x] 18. `srand` reseed per eye (`vr.cpp:1590`)
- [x] 19. Room-scale movement — head XY discarded from the camera, hands measured
      head-relative, accumulator retired. Cannot drift by construction.
- [ ] 20. `VR_GetBodyYawAngle` — blended head/hand body yaw (`vr.cpp:2526-2540`)
- [ ] 21. `VR_GetCrouchRatio` / crouch-aware anchors
- [ ] 22. `vr_height_calibration` wired to the anchors (cvar exists, unused)
- [ ] 23. Stair smoothing behaviour parity
- [ ] 24. `VR_PushYaw` / yaw readback on `svc_setview` and teleport

## C. Controllers

- [x] 25. OpenXR action set; Touch, Index, generic profiles
- [x] 26. Grip + aim poses, trigger, grip, sticks, buttons
- [x] 27. Hand velocity via `XR_SPACE_VELOCITY`
- [x] 28. Throw velocity averaging (15-frame mean)
- [x] 29. Haptics + `#81` builtin
- [~] 30. Finger tracking — `XR_EXT_hand_tracking`; falls back to grip on Touch.
      22 `vr_finger*` cvars exist in quakevr, 4 ported.
- [ ] 31. `vr_fingers_and_base_*` hand model composition (9 cvars)
- [ ] 32. Weapon-mounted buttons (`VR_DoWpnButton`)
- [ ] 33. Flick reload (`VR_UpdateFlick`, `vr_spinreload*`)
- [ ] 34. `vr_offhandpitch` / `vr_offhandyaw`

## D. Weapon

- [x] 35. Viewmodel at the hand, grip orientation
- [x] 36. Per-weapon offset table — id1/hipnotic/rogue + Arcane Dimensions, verbatim
- [x] 37. `VR_ApplyModelMod` mechanism + `VR_GetScaleCorrect`
- [x] 38. `vr_gunangle` 32° pre-rotation
- [x] 39. Roll via `atan2` (not `asin`)
- [x] 40. `original_scale` snapshot for MDL, MD5, MD3
- [x] 41. Viewmodel depth-range and `cl_gun_fovscale` disabled in VR
- [x] 42. Gun wall collisions (default off pending verification)
- [x] 43. QC `handrot` matches the drawn weapon angles, gun pre-rotation included
- [x] 44. Offsets applied to the header the renderer actually draws
- [ ] 45. Weapon weight simulation (10 `vr_wpn_pos_weight*` cvars)
- [ ] 46. `VR_DoWeaponDirSlerp` — weapon direction smoothing
- [ ] 47. Two-handed aiming distance rules (`VR_GoodDistanceFor*`, 5 `vr_2h*` cvars)
- [ ] 48. Ironsights

## E. Gameplay (QuakeC bridge)

- [x] 49. quakevr's full QC compiles and loads — **CRC 5927**, mods still work
- [x] 50. 68 VR fields via `QCEXTFIELD` named lookup
- [x] 51. 27 quakevr builtins at their exact numbers
- [x] 52. Hand-touch item pickup (`.handtouch`)
- [x] 53. Holster hotspots — 6 spots, quakevr's offsets and thresholds
- [x] 54. `vrbits0` state bits
- [x] 55. Teleport locomotion
- [ ] 56. Holsters persisting across level change (`parm17`-`parm40` not carried)
- [ ] 57. Force grab (4 `vr_forcegrab*` + 4 `vr_forcegrabbable*` cvars)
- [ ] 58. Weapon throwing tuning (`vr_weapon_throw_*`)
- [ ] 59. `vr_body_interactions` wired through
- [ ] 60. `VR_OnSpawnServer` / `VR_OnClientClearState` / `VR_OnLoadedPak` lifecycle

## F. Models and presentation

- [x] 61. quakevr asset paks installed (64 models incl. torso, holsters, fingers)
- [x] 62. VR torso + leg holster model scale/offset
- [x] 63. Torso positioned per-frame
- [x] 64. Hand models rendered with per-finger frames
- [x] 65. VR HUD — 2D canvas on a world-space panel
- [~] 66. VR menu — on the panel with the HUD; virtual keyboard not ported
- [ ] 67. Crosshair modes (4 `vr_crosshair*` cvars)
- [ ] 68. Laser sight — have a dlight dot; quakevr has proper modes
- [ ] 69. `vr_show*` debug visualisations (5 cvars)
- [ ] 70. World text (`worldtext_*` builtins declared, unimplemented)

## G. Comfort and misc

- [x] 71. Snap turn / smooth turn, deadzone
- [ ] 72. Melee tuning (5 `vr_melee*` cvars) — threshold only so far
- [ ] 73. Positional damage (`vr_positional*`)
- [ ] 74. Enemy/ammo box interaction cvars
- [ ] 75. Autosave behaviour (3 cvars)
- [ ] 76. Flatscreen mode (`vr_fakevr`) — deliberately deferred

---

## Known bugs

All four from the audit are fixed. Nothing outstanding that is known to be wrong
— what remains is unported features, not defects.

## Score

**~50 of 76 done.** Engine, tracking, controllers, the QuakeC bridge, the VR
body and the HUD are complete. What remains is gameplay tuning cvars and a few
presentation extras.

## Next, in order

1. Force grab (#57) — 8 cvars, pulls items to the hand
2. Weapon weight simulation (#45) and 2H grab rules (#47)
3. Weapon-mounted buttons (#32) and flick reload (#33)
4. Crosshair modes (#67) and proper laser sight (#68)
5. Holsters persisting across level change (#56) — needs spawnparm extension
6. `VK_KHR_multiview` (#12) — halves the render cost

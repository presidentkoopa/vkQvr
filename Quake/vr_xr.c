/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

See file, 'COPYING', for details.
*/
// vr_xr.c -- OpenXR runtime integration
//
// Stage 1: acquire an OpenXR instance and HMD system, and report what the
// runtime wants from Vulkan. Session, swapchains and stereo submission come
// later; nothing here touches the renderer yet.

#include "quakedef.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vr_xr.h"

qboolean vr_xr_active = false;
uint32_t vr_xr_eye_width = 0;
uint32_t vr_xr_eye_height = 0;

static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system = XR_NULL_SYSTEM_ID;

// Quake is a stereo, seated/standing HMD app; this is the only view config we support.
#define XR_VIEW_CONFIG XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO

// defined further down with the session statics
static void XR_DestroySession (void);
static void XR_AcquireEyes (void);
static void XR_UpdateRoomscale (void);
static void XR_InitHandTracking (void);
void		VR_XR_ModAllModels (void);
static int	XR_ComputeHotSpot (const vec3_t hand_world, const vec3_t player_origin);
static void VR_XR_Calibrate_f (void);
static void VR_XR_Recenter_f (void);
static void VR_XR_ScaleDump_f (void);
static void VR_XR_BodyDump_f (void);
static float XR_CrouchRatio (void);

// Finger tracking cvars, declared here because VR_XR_Init registers them well
// before the finger-tracking section defines them. Defaults are quakevr's
// (vr_cvars.cpp:173-187).
cvar_t vr_gun_debug = {"vr_gun_debug", "0", CVAR_NONE};

// quakevr's per-weapon offset table. On: the offsets are required, not
// cosmetic.
//
// I had this off for a while on the theory that the offsets encoded quakevr's
// controller frame and so could not port. Measuring the models says otherwise.
// The offsets are overwhelmingly shrink-compensation along each model's own
// axes: a model is scaled about its bounding-box min, so shrinking it pulls it
// away from the hand by about (1 - scale) * extent, and the offset puts it
// back. Hence the inverse correlation through the whole table -- scale 0.8
// wants ofs_z 8.5, scale 0.5 wants 13-19, scale 0.33 wants 37, scale 0.25
// wants 41. Drop them and the heavily-shrunk weapons are the worst off.
//
// vkQuake consumes scale_origin exactly as quakevr does, translate then scale
// (r_alias.c:533-538 against quakevr's trMat), so the numbers carry over as-is.
//
// What the offsets are is an amplifier: scale_origin is rotated by the entity
// angles, so with offsets reaching 41 units any error in the weapon's rotation
// is magnified into inches or feet of displacement. If the weapon sits wrong,
// suspect the rotation and not this table.
cvar_t vr_wpn_offsets = {"vr_wpn_offsets", "1", CVAR_ARCHIVE};

// Pre-rotation of the weapon relative to the controller. quakevr applies this
// to the controller matrix before deriving handrot, and every offset in the
// per-weapon table was tuned in that rotated frame.
// (quakevr vr.cpp:3137-3155; defaults vr_cvars.cpp:54, 69)
/*
================================================================================

	WEAPON WEIGHT

	A heavy weapon should lag the controller rather than track it exactly.
	quakevr does that by interpolating the hand position toward the tracked
	position, and slerping the hand direction toward the tracked direction, by a
	factor derived from the weapon's weight (vr.cpp VR_GetWeaponWeightFactorImpl
	and VR_DoWeaponDirSlerp).

	Worth knowing before tuning: at stock settings this does nothing at all,
	in quakevr as much as here. Per-weapon Weight is the 21st argument of
	InitWeaponCVars and no call passes it, so it is 0.0 for every weapon, the
	factor clamps to 1.0, and the frametime-adjusted blend runs at about 1.11 at
	90fps -- a complete blend to the tracked pose every frame. The machinery is
	here to be dialled in, and until it is, the weapon tracks 1:1 exactly as
	before.

================================================================================
*/

cvar_t vr_wpn_pos_weight = {"vr_wpn_pos_weight", "1", CVAR_ARCHIVE};
cvar_t vr_wpn_pos_weight_offset = {"vr_wpn_pos_weight_offset", "0.0", CVAR_ARCHIVE};
cvar_t vr_wpn_pos_weight_mult = {"vr_wpn_pos_weight_mult", "1.0", CVAR_ARCHIVE};
cvar_t vr_wpn_pos_weight_2h_help_offset = {"vr_wpn_pos_weight_2h_help_offset", "0.3", CVAR_ARCHIVE};
cvar_t vr_wpn_pos_weight_2h_help_mult = {"vr_wpn_pos_weight_2h_help_mult", "1.0", CVAR_ARCHIVE};
cvar_t vr_wpn_dir_weight = {"vr_wpn_dir_weight", "1", CVAR_ARCHIVE};
cvar_t vr_wpn_dir_weight_offset = {"vr_wpn_dir_weight_offset", "0.05", CVAR_ARCHIVE};
cvar_t vr_wpn_dir_weight_mult = {"vr_wpn_dir_weight_mult", "1", CVAR_ARCHIVE};
cvar_t vr_wpn_dir_weight_2h_help_offset = {"vr_wpn_dir_weight_2h_help_offset", "0.3", CVAR_ARCHIVE};
cvar_t vr_wpn_dir_weight_2h_help_mult = {"vr_wpn_dir_weight_2h_help_mult", "1.0", CVAR_ARCHIVE};

// Model-only pitch. quakevr applies this to the drawn weapon and nothing else
// (VR_GetWpnAngleOffsets, vr.cpp:815, used by CalcGunAngle at view.cpp:706),
// so it rotates the model without moving the aim direction the shot follows.
// That is the knob for a weapon that fires true but hangs at the wrong angle.
cvar_t vr_gunmodelpitch = {"vr_gunmodelpitch", "7", CVAR_ARCHIVE};

// Hand size. quakevr never scales hand_base.mdl and its shipped finger offsets
// are tuned against an unscaled palm, so 1.0 is its real look. Kept as a knob
// because these hands are large: 13.1 units, 0.40m at the shipped world scale.
// Old comment follows, from when this was derived at the wrong world scale.
// quakevr has no equivalent -- it never scales hand_base.mdl at all,
// registering only hand.mdl at scale 0 to keep the weapon placeholder hidden.
//
// The default is derived from quakevr's own sizing rule rather than picked.
// Its weapon table brings models to true size at 26.25 units to the metre:
// v_shot.mdl measures 40.1 units and the table's 0.5 scale with the 1.333
// correction takes it to 26.7 units, which is 1.02m -- a real shotgun. The
// same rule on hand_base.mdl, 13.1 units or half a metre unscaled, wants
// roughly 5 units for a hand, hence 0.38.
// Anchor the hand to a vertex of the weapon it holds, instead of leaving it at
// the controller. quakevr always does this (V_SetupHandViewEnt, view.cpp:1411)
// and its weapon models are re-authored, so vertex 0 is a deliberate grip
// point rather than an arbitrary one -- HandAnchorVertex defaults to 0 for
// every weapon and no InitWeaponCVars call overrides it.
//
// Off for models that were never authored for it, where vertex 0 means nothing
// in particular. vr_grip_vertex picks the vertex: -1 finds it from the mesh,
// and any value from 0 up pins one explicitly.
cvar_t vr_hand_grips_weapon = {"vr_hand_grips_weapon", "1", CVAR_ARCHIVE};
// -1 means work the grip out from the mesh; 0 or above pins a specific vertex.
cvar_t vr_grip_vertex = {"vr_grip_vertex", "-1", CVAR_ARCHIVE};

cvar_t vr_hand_scale = {"vr_hand_scale", "1.0", CVAR_ARCHIVE};

// Measured in the headset: the OpenXR aim pose on this hardware points about
// 30 degrees above where the controller actually is, so everything derived from
// it -- models and shots both -- needs rotating back down by that much.
// quakevr ships 32 against the OpenVR controller pose, a different frame.
cvar_t vr_gunangle = {"vr_gunangle", "-30", CVAR_ARCHIVE};
cvar_t vr_gunyaw = {"vr_gunyaw", "0", CVAR_ARCHIVE};
// Roll to go with the pitch and yaw. quakevr needs only two axes because its
// controller pose and its models share a frame; the OpenXR aim pose is a
// pointing ray with its own roll about the barrel, so the third axis is needed
// to seat the models in the hand.
// Two-handed aiming. quakevr's numbers (vr_cvars.cpp:88-95, 174).
// vr_2h_mode: 0 off, 1 hands only, 2 hands plus virtual stock.
cvar_t vr_2h_mode = {"vr_2h_mode", "2", CVAR_ARCHIVE};
cvar_t vr_2h_angle_threshold = {"vr_2h_angle_threshold", "0.65", CVAR_ARCHIVE};
cvar_t vr_2h_disable_angle_threshold = {"vr_2h_disable_angle_threshold", "0", CVAR_ARCHIVE};
cvar_t vr_2h_virtual_stock_factor = {"vr_2h_virtual_stock_factor", "0.5", CVAR_ARCHIVE};
cvar_t vr_virtual_stock_thresh = {"vr_virtual_stock_thresh", "10", CVAR_ARCHIVE};
cvar_t vr_show_virtual_stock = {"vr_show_virtual_stock", "0", CVAR_ARCHIVE};

/*
================================================================================

	THE REMAINDER OF QUAKEVR'S CVARS

	Defaults are quakevr's, from vr_cvars.cpp. Some of these drive code below;
	the rest exist because the QuakeC and the menus look them up by name, and a
	name that does not resolve reads as 0 rather than as its default -- which is
	how force grab and half a dozen other features sat dead earlier.

================================================================================
*/

// aiming and movement modes. 6 is quakevr's controller-aiming default.
cvar_t vr_aimmode = {"vr_aimmode", "6", CVAR_ARCHIVE};
cvar_t vr_movement_mode = {"vr_movement_mode", "1", CVAR_ARCHIVE};

// room-scale jumping: rise fast enough, from high enough, and the player jumps
cvar_t vr_roomscale_jump = {"vr_roomscale_jump", "1", CVAR_ARCHIVE};
cvar_t vr_roomscale_jump_threshold = {"vr_roomscale_jump_threshold", "0.8", CVAR_ARCHIVE};
cvar_t vr_roomscale_move_mult = {"vr_roomscale_move_mult", "1", CVAR_ARCHIVE};

// throwing. 0 uses the averaged hand velocity, 1 adds the spin of the wrist.
cvar_t vr_throw_algorithm = {"vr_throw_algorithm", "1", CVAR_ARCHIVE};
cvar_t vr_throw_up_center_of_mass = {"vr_throw_up_center_of_mass", "0.01", CVAR_ARCHIVE};

// the off hand can be angled differently from the main one
cvar_t vr_offhandpitch = {"vr_offhandpitch", "40.25", CVAR_ARCHIVE};
cvar_t vr_offhandyaw = {"vr_offhandyaw", "-4", CVAR_ARCHIVE};

// comfort and input
cvar_t vr_viewkick = {"vr_viewkick", "0", CVAR_NONE};
cvar_t vr_disablehaptics = {"vr_disablehaptics", "0", CVAR_ARCHIVE};
cvar_t vr_enable_joystick_turn = {"vr_enable_joystick_turn", "1", CVAR_ARCHIVE};
cvar_t vr_player_shadows = {"vr_player_shadows", "2", CVAR_ARCHIVE};
cvar_t vr_msaa = {"vr_msaa", "4", CVAR_ARCHIVE};
cvar_t vr_novrinit = {"vr_novrinit", "0", CVAR_NONE};
cvar_t vr_fakevr_handroll = {"vr_fakevr_handroll", "0", CVAR_NONE};

// world-space HUD and status bar
cvar_t vr_hud_scale = {"vr_hud_scale", "0.025", CVAR_ARCHIVE};
cvar_t vr_sbar_mode = {"vr_sbar_mode", "1", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_x = {"vr_sbar_offset_x", "-12", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_y = {"vr_sbar_offset_y", "1", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_z = {"vr_sbar_offset_z", "-3", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_pitch = {"vr_sbar_offset_pitch", "1", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_yaw = {"vr_sbar_offset_yaw", "1.6", CVAR_ARCHIVE};
cvar_t vr_sbar_offset_roll = {"vr_sbar_offset_roll", "-0.3", CVAR_ARCHIVE};
cvar_t vr_menumode = {"vr_menumode", "0", CVAR_ARCHIVE};
cvar_t vr_menu_mouse_pointer_hand = {"vr_menu_mouse_pointer_hand", "1", CVAR_ARCHIVE};

// crosshair
cvar_t vr_crosshair = {"vr_crosshair", "0", CVAR_ARCHIVE};
cvar_t vr_crosshair_depth = {"vr_crosshair_depth", "0", CVAR_ARCHIVE};
cvar_t vr_crosshair_size = {"vr_crosshair_size", "1", CVAR_ARCHIVE};
cvar_t vr_crosshair_alpha = {"vr_crosshair_alpha", "0.85", CVAR_ARCHIVE};
cvar_t vr_crosshairy = {"vr_crosshairy", "0", CVAR_ARCHIVE};

// flick reload: spin the wrist hard enough about X and the weapon reloads
cvar_t vr_spinreload_pitch_speed = {"vr_spinreload_pitch_speed", "1100", CVAR_ARCHIVE};
cvar_t vr_spinreload_x_angular_threshold = {"vr_spinreload_x_angular_threshold", "6.5", CVAR_ARCHIVE};

// autosave
cvar_t vr_autosave_seconds = {"vr_autosave_seconds", "240", CVAR_ARCHIVE};
cvar_t vr_autosave_on_changelevel = {"vr_autosave_on_changelevel", "1", CVAR_ARCHIVE};
cvar_t vr_autosave_show_message = {"vr_autosave_show_message", "0", CVAR_ARCHIVE};

// debug visualisations
cvar_t vr_show_hip_holsters = {"vr_show_hip_holsters", "1", CVAR_ARCHIVE};
cvar_t vr_show_shoulder_holsters = {"vr_show_shoulder_holsters", "1", CVAR_ARCHIVE};
cvar_t vr_show_upper_holsters = {"vr_show_upper_holsters", "1", CVAR_ARCHIVE};
cvar_t vr_show_weapon_text = {"vr_show_weapon_text", "1", CVAR_ARCHIVE};
cvar_t vr_vrtorso_debuglines_enabled = {"vr_vrtorso_debuglines_enabled", "0", CVAR_ARCHIVE};
cvar_t vr_debug_print_handvel = {"vr_debug_print_handvel", "0", CVAR_ARCHIVE};
cvar_t vr_debug_print_headvel = {"vr_debug_print_headvel", "0", CVAR_ARCHIVE};
cvar_t vr_debug_show_hand_pos_and_rot = {"vr_debug_show_hand_pos_and_rot", "0", CVAR_ARCHIVE};

cvar_t vr_activestartpaknameidx = {"vr_activestartpaknameidx", "0", CVAR_ARCHIVE};

cvar_t vr_gunroll = {"vr_gunroll", "0", CVAR_ARCHIVE};
cvar_t vr_gun_z_offset = {"vr_gun_z_offset", "-1", CVAR_ARCHIVE};
/*
================================================================================

	CVARS READ BY THE QUAKEC

	quakevr implements a lot of its gameplay in QuakeC rather than the engine,
	and that QC reaches back for its tuning through cvar_hmake/cvar_hget. Every
	name below is one the ported QC binds in VR_CVars_InitAllHandles: without an
	engine-side cvar to bind to, the handle resolves to nothing and the feature
	reads as 0 and quietly does nothing.

	That was the state of force grab, melee tuning, positional damage, enemy and
	ammo box drops, headbutting, holster mode, reload mode, weapon cycling and
	2H spread -- all of them already compiled into the QC and all of them dead.
	vr_enabled mattered most of all, being the flag the QC tests to decide
	whether it is in VR at all.

	Defaults are quakevr's, from vr_cvars.cpp.

================================================================================
*/

// The QC's "am I in VR" flag. Not archived, and not user-set: the engine
// drives it from the real session state. (vr_cvars.cpp, DEFINE_FCVAR)
cvar_t vr_enabled = {"vr_enabled", "0", CVAR_NONE};
cvar_t vr_fakevr = {"vr_fakevr", "0", CVAR_NONE};

// force grab -- pulling items to the hand
cvar_t vr_forcegrab_mode = {"vr_forcegrab_mode", "1", CVAR_ARCHIVE};
cvar_t vr_forcegrab_powermult = {"vr_forcegrab_powermult", "0.75", CVAR_ARCHIVE};
cvar_t vr_forcegrab_range = {"vr_forcegrab_range", "150.0", CVAR_ARCHIVE};
cvar_t vr_forcegrab_radius = {"vr_forcegrab_radius", "18", CVAR_ARCHIVE};
cvar_t vr_forcegrab_eligible_particles = {"vr_forcegrab_eligible_particles", "1", CVAR_ARCHIVE};
cvar_t vr_forcegrab_eligible_haptics = {"vr_forcegrab_eligible_haptics", "1", CVAR_ARCHIVE};
cvar_t vr_forcegrabbable_ammo_boxes = {"vr_forcegrabbable_ammo_boxes", "1", CVAR_ARCHIVE};
cvar_t vr_forcegrabbable_health_boxes = {"vr_forcegrabbable_health_boxes", "1", CVAR_ARCHIVE};
cvar_t vr_forcegrabbable_return_time_deathmatch = {"vr_forcegrabbable_return_time_deathmatch", "4", CVAR_ARCHIVE};
cvar_t vr_forcegrabbable_return_time_singleplayer = {"vr_forcegrabbable_return_time_singleplayer", "0", CVAR_ARCHIVE};

// melee
cvar_t vr_melee_dmg_multiplier = {"vr_melee_dmg_multiplier", "1.0", CVAR_ARCHIVE};
cvar_t vr_melee_range_multiplier = {"vr_melee_range_multiplier", "1.0", CVAR_ARCHIVE};
cvar_t vr_melee_bloodlust = {"vr_melee_bloodlust", "0", CVAR_ARCHIVE};
cvar_t vr_melee_bloodlust_mult = {"vr_melee_bloodlust_mult", "1.0", CVAR_ARCHIVE};

// headbutting
cvar_t vr_headbutt_damage_mult = {"vr_headbutt_damage_mult", "32", CVAR_ARCHIVE};
cvar_t vr_headbutt_velocity_threshold = {"vr_headbutt_velocity_threshold", "2.02", CVAR_ARCHIVE};

// drops from enemies and ammo boxes
cvar_t vr_enemy_drops = {"vr_enemy_drops", "0", CVAR_ARCHIVE};
cvar_t vr_enemy_drops_chance_mult = {"vr_enemy_drops_chance_mult", "1", CVAR_ARCHIVE};
cvar_t vr_ammobox_drops = {"vr_ammobox_drops", "0", CVAR_ARCHIVE};
cvar_t vr_ammobox_drops_chance_mult = {"vr_ammobox_drops_chance_mult", "1", CVAR_ARCHIVE};

// handling
cvar_t vr_positional_damage = {"vr_positional_damage", "1", CVAR_ARCHIVE};
cvar_t vr_holster_mode = {"vr_holster_mode", "0", CVAR_ARCHIVE};
cvar_t vr_holster_haptics = {"vr_holster_haptics", "2", CVAR_ARCHIVE};
cvar_t vr_reload_mode = {"vr_reload_mode", "2", CVAR_ARCHIVE};
cvar_t vr_weapon_cycle_mode = {"vr_weapon_cycle_mode", "0", CVAR_ARCHIVE};
cvar_t vr_weapondrop_particles = {"vr_weapondrop_particles", "1", CVAR_ARCHIVE};
cvar_t vr_2h_spread_reduction = {"vr_2h_spread_reduction", "0.5", CVAR_ARCHIVE};
cvar_t vr_2h_throw_velocity_mult = {"vr_2h_throw_velocity_mult", "1.4", CVAR_ARCHIVE};
cvar_t vr_verbosebots = {"vr_verbosebots", "0", CVAR_ARCHIVE};

/*
	HAND ASSEMBLY OFFSETS

	The hand is not one model: it is hand_base.mdl plus five finger models, each
	positioned separately. quakevr composes them through a chain of offsets that
	accumulate (view.cpp fingerIdxToOffset) -- a term for the whole hand, then
	one for the fingers as a group, then one per finger, with an extra term
	applied to the off hand so the two can be tuned apart.

	Every one defaults to 0.0, so out of the box the parts sit exactly where the
	models put them. They exist to be dialled in.

================================================================================
*/

// whole hand: base and fingers together
cvar_t vr_fingers_and_base_x = {"vr_fingers_and_base_x", "1.925", CVAR_ARCHIVE};
cvar_t vr_fingers_and_base_y = {"vr_fingers_and_base_y", "-2.825", CVAR_ARCHIVE};
cvar_t vr_fingers_and_base_z = {"vr_fingers_and_base_z", "-2.075", CVAR_ARCHIVE};

// added on top for the off hand only
cvar_t vr_fingers_and_base_offhand_x = {"vr_fingers_and_base_offhand_x", "0", CVAR_ARCHIVE};
cvar_t vr_fingers_and_base_offhand_y = {"vr_fingers_and_base_offhand_y", "0", CVAR_ARCHIVE};
cvar_t vr_fingers_and_base_offhand_z = {"vr_fingers_and_base_offhand_z", "0.0", CVAR_ARCHIVE};

// all five fingers as a group, not the palm
cvar_t vr_fingers_x = {"vr_fingers_x", "-5.05", CVAR_ARCHIVE};
cvar_t vr_fingers_y = {"vr_fingers_y", "-0.1", CVAR_ARCHIVE};
cvar_t vr_fingers_z = {"vr_fingers_z", "-0.1875", CVAR_ARCHIVE};

// the palm alone
cvar_t vr_finger_base_x = {"vr_finger_base_x", "0", CVAR_ARCHIVE};
cvar_t vr_finger_base_y = {"vr_finger_base_y", "0.0", CVAR_ARCHIVE};
cvar_t vr_finger_base_z = {"vr_finger_base_z", "0.0", CVAR_ARCHIVE};

// and one finger at a time
cvar_t vr_finger_thumb_x = {"vr_finger_thumb_x", "-0.3625", CVAR_ARCHIVE};
cvar_t vr_finger_thumb_y = {"vr_finger_thumb_y", "3.2625", CVAR_ARCHIVE};
cvar_t vr_finger_thumb_z = {"vr_finger_thumb_z", "-1.9375", CVAR_ARCHIVE};
cvar_t vr_finger_index_x = {"vr_finger_index_x", "-0.325", CVAR_ARCHIVE};
cvar_t vr_finger_index_y = {"vr_finger_index_y", "0.6125", CVAR_ARCHIVE};
cvar_t vr_finger_index_z = {"vr_finger_index_z", "-1.825", CVAR_ARCHIVE};
cvar_t vr_finger_middle_x = {"vr_finger_middle_x", "-0.3625", CVAR_ARCHIVE};
cvar_t vr_finger_middle_y = {"vr_finger_middle_y", "0.5125", CVAR_ARCHIVE};
cvar_t vr_finger_middle_z = {"vr_finger_middle_z", "-0.3125", CVAR_ARCHIVE};
cvar_t vr_finger_ring_x = {"vr_finger_ring_x", "-0.3625", CVAR_ARCHIVE};
cvar_t vr_finger_ring_y = {"vr_finger_ring_y", "0.7", CVAR_ARCHIVE};
cvar_t vr_finger_ring_z = {"vr_finger_ring_z", "0.65", CVAR_ARCHIVE};
cvar_t vr_finger_pinky_x = {"vr_finger_pinky_x", "-0.325", CVAR_ARCHIVE};
cvar_t vr_finger_pinky_y = {"vr_finger_pinky_y", "1.15", CVAR_ARCHIVE};
cvar_t vr_finger_pinky_z = {"vr_finger_pinky_z", "1.6375", CVAR_ARCHIVE};

cvar_t vr_finger_grip_bias = {"vr_finger_grip_bias", "0", CVAR_ARCHIVE};
cvar_t vr_finger_auto_close_thumb = {"vr_finger_auto_close_thumb", "1", CVAR_ARCHIVE};
cvar_t vr_finger_blending = {"vr_finger_blending", "1", CVAR_ARCHIVE};
cvar_t vr_finger_blending_speed = {"vr_finger_blending_speed", "50", CVAR_ARCHIVE};

// Teleport locomotion (quakevr vr_cvars.cpp:86-87). State lives here rather
// than beside the implementation because the edict-field writer reads it.
cvar_t vr_teleport_enabled = {"vr_teleport_enabled", "1", CVAR_ARCHIVE};
cvar_t vr_teleport_range = {"vr_teleport_range", "400", CVAR_ARCHIVE};

static qboolean xr_teleporting = false;
static qboolean xr_was_teleporting = false;
static qboolean xr_teleport_valid = false;
static vec3_t	xr_teleport_impact;
static qboolean xr_send_teleport = false;

// Room-scale bookkeeping. Head position in play space (Quake units) last frame,
// the delta to hand to the movement command, and the running total already
// turned into player movement -- the view subtracts that, otherwise a step
// forward moves both player and camera and you travel twice as far as you
// walked. (quakevr vr.cpp:2691-2692)
static vec3_t	xr_last_head_pos;
// (retired: the old room-scale accumulator -- see XR_UpdateRoomscale)
static vec3_t	xr_roomscale_delta;
static qboolean xr_head_pos_valid = false;

// Head yaw within play space, i.e. excluding whatever the player has turned to
// with the stick. Movement is head-relative, but the server derives its
// movement basis from the angles we send it -- which are the hand's when hand
// aiming is on -- so this is needed to correct for the difference.
static float xr_head_yaw = 0.0f;

// mathlib.h only provides the one direction
#define RAD2DEG(a) ((a) / M_PI_DIV_180)

/*
XR_ResultStr

xrResultToString needs a live instance, so fall back to the numeric code when
we fail before (or while) creating one.
===============
*/
static const char *XR_ResultStr (XrResult res)
{
	static char buf[XR_MAX_RESULT_STRING_SIZE];
	if (xr_instance != XR_NULL_HANDLE && XR_SUCCEEDED (xrResultToString (xr_instance, res, buf)))
		return buf;
	q_snprintf (buf, sizeof (buf), "XrResult %d", (int)res);
	return buf;
}

/*
XR_HasExtension
===============
*/
static qboolean XR_HasExtension (const XrExtensionProperties *props, uint32_t count, const char *name)
{
	uint32_t i;
	for (i = 0; i < count; i++)
	{
		if (!strcmp (props[i].extensionName, name))
			return true;
	}
	return false;
}

/*
VR_XR_Shutdown
===============
*/
void VR_XR_Shutdown (void)
{
	XR_DestroySession ();

	if (xr_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance (xr_instance);
		xr_instance = XR_NULL_HANDLE;
	}
	xr_system = XR_NULL_SYSTEM_ID;
	vr_xr_active = false;
}

/*
VR_XR_Init

Never fatal. Any failure logs and leaves the engine running flat.
===============
*/
void VR_XR_Init (void)
{
	XrResult				   res;
	uint32_t				   ext_count = 0;
	XrExtensionProperties	  *ext_props = NULL;
	uint32_t				   i;
	XrInstanceCreateInfo	   instance_info;
	XrApplicationInfo		  *app;
	XrSystemGetInfo			   system_info;
	XrSystemProperties		   system_props;
	XrInstanceProperties	   instance_props;
	uint32_t				   view_count = 0;
	XrViewConfigurationView	   views[2];
	const char				  *enabled_exts[2];
	uint32_t				   num_enabled_exts = 0;
	PFN_xrGetVulkanGraphicsRequirements2KHR pfn_get_vk_reqs = NULL;
	XrGraphicsRequirementsVulkanKHR vk_reqs;

	// registered unconditionally so it can be set from configs even when the
	// engine happens to start flat
	Cvar_RegisterVariable (&vr_world_scale);
	Cvar_RegisterVariable (&vr_floor_offset);
	Cvar_RegisterVariable (&vr_turn_speed);
	Cvar_RegisterVariable (&vr_snap_turn);
	Cvar_RegisterVariable (&vr_deadzone);
	Cvar_RegisterVariable (&vr_roomscale);
	Cvar_RegisterVariable (&vr_roomscale_mult);
	Cvar_RegisterVariable (&vr_hand_aiming);
	Cvar_RegisterVariable (&vr_aim_hand);
	Cvar_RegisterVariable (&vr_gun_offset_x);
	Cvar_RegisterVariable (&vr_gun_offset_y);
	Cvar_RegisterVariable (&vr_gun_offset_z);
	Cvar_RegisterVariable (&vr_melee_threshold);
	Cvar_RegisterVariable (&vr_haptics);
	Cvar_RegisterVariable (&vr_laser);
	Cvar_RegisterVariable (&vr_2h_mode);
	Cvar_RegisterVariable (&vr_2h_angle_threshold);
	Cvar_RegisterVariable (&vr_2h_disable_angle_threshold);
	Cvar_RegisterVariable (&vr_2h_virtual_stock_factor);
	Cvar_RegisterVariable (&vr_virtual_stock_thresh);
	Cvar_RegisterVariable (&vr_show_virtual_stock);
	Cvar_RegisterVariable (&vr_two_handed);
	Cvar_RegisterVariable (&vr_two_hand_dist);
	Cvar_RegisterVariable (&vr_height_calibration);
	Cvar_RegisterVariable (&vr_gunmodelscale);
	Cvar_RegisterVariable (&vr_gunmodely);
	Cvar_RegisterVariable (&vr_body_interactions);
	Cvar_RegisterVariable (&vr_throw_avg_frames);
	Cvar_RegisterVariable (&vr_throw_angvel_avg_frames);
	Cvar_RegisterVariable (&vr_weapon_throw_velocity_mult);
	Cvar_RegisterVariable (&vr_weapon_throw_damage_mult);
	Cvar_RegisterVariable (&vr_weapon_throw_mode);

	Cvar_RegisterVariable (&vr_vrtorso_enabled);
	Cvar_RegisterVariable (&vr_vrtorso_x_offset);
	Cvar_RegisterVariable (&vr_vrtorso_y_offset);
	Cvar_RegisterVariable (&vr_vrtorso_z_offset);
	Cvar_RegisterVariable (&vr_vrtorso_head_z_mult);
	Cvar_RegisterVariable (&vr_vrtorso_x_scale);
	Cvar_RegisterVariable (&vr_vrtorso_y_scale);
	Cvar_RegisterVariable (&vr_vrtorso_z_scale);
	Cvar_RegisterVariable (&vr_vrtorso_pitch);
	Cvar_RegisterVariable (&vr_vrtorso_yaw);
	Cvar_RegisterVariable (&vr_vrtorso_roll);
	Cvar_RegisterVariable (&vr_leg_holster_model_enabled);
	Cvar_RegisterVariable (&vr_leg_holster_model_scale);
	Cvar_RegisterVariable (&vr_leg_holster_model_x_offset);
	Cvar_RegisterVariable (&vr_leg_holster_model_y_offset);
	Cvar_RegisterVariable (&vr_leg_holster_model_z_offset);

	Cvar_RegisterVariable (&vr_fingers_and_base_x);
	Cvar_RegisterVariable (&vr_fingers_and_base_y);
	Cvar_RegisterVariable (&vr_fingers_and_base_z);
	Cvar_RegisterVariable (&vr_fingers_and_base_offhand_x);
	Cvar_RegisterVariable (&vr_fingers_and_base_offhand_y);
	Cvar_RegisterVariable (&vr_fingers_and_base_offhand_z);
	Cvar_RegisterVariable (&vr_fingers_x);
	Cvar_RegisterVariable (&vr_fingers_y);
	Cvar_RegisterVariable (&vr_fingers_z);
	Cvar_RegisterVariable (&vr_finger_base_x);
	Cvar_RegisterVariable (&vr_finger_base_y);
	Cvar_RegisterVariable (&vr_finger_base_z);
	Cvar_RegisterVariable (&vr_finger_thumb_x);
	Cvar_RegisterVariable (&vr_finger_thumb_y);
	Cvar_RegisterVariable (&vr_finger_thumb_z);
	Cvar_RegisterVariable (&vr_finger_index_x);
	Cvar_RegisterVariable (&vr_finger_index_y);
	Cvar_RegisterVariable (&vr_finger_index_z);
	Cvar_RegisterVariable (&vr_finger_middle_x);
	Cvar_RegisterVariable (&vr_finger_middle_y);
	Cvar_RegisterVariable (&vr_finger_middle_z);
	Cvar_RegisterVariable (&vr_finger_ring_x);
	Cvar_RegisterVariable (&vr_finger_ring_y);
	Cvar_RegisterVariable (&vr_finger_ring_z);
	Cvar_RegisterVariable (&vr_finger_pinky_x);
	Cvar_RegisterVariable (&vr_finger_pinky_y);
	Cvar_RegisterVariable (&vr_finger_pinky_z);
	Cvar_RegisterVariable (&vr_finger_grip_bias);
	Cvar_RegisterVariable (&vr_finger_auto_close_thumb);
	Cvar_RegisterVariable (&vr_finger_blending);
	Cvar_RegisterVariable (&vr_finger_blending_speed);
	Cvar_RegisterVariable (&vr_teleport_enabled);
	Cvar_RegisterVariable (&vr_teleport_range);
	Cvar_RegisterVariable (&vr_gun_wall_collision);
	Cvar_RegisterVariable (&vr_gun_debug);
	Cvar_RegisterVariable (&vr_wpn_offsets);
	Cvar_RegisterVariable (&vr_hud_enabled);
	Cvar_RegisterVariable (&vr_menu_scale);
	Cvar_RegisterVariable (&vr_menu_distance);
	Cvar_RegisterVariable (&vr_lefthanded);
	Cvar_RegisterVariable (&vr_wpn_pos_weight);
	Cvar_RegisterVariable (&vr_wpn_pos_weight_offset);
	Cvar_RegisterVariable (&vr_wpn_pos_weight_mult);
	Cvar_RegisterVariable (&vr_wpn_pos_weight_2h_help_offset);
	Cvar_RegisterVariable (&vr_wpn_pos_weight_2h_help_mult);
	Cvar_RegisterVariable (&vr_wpn_dir_weight);
	Cvar_RegisterVariable (&vr_wpn_dir_weight_offset);
	Cvar_RegisterVariable (&vr_wpn_dir_weight_mult);
	Cvar_RegisterVariable (&vr_wpn_dir_weight_2h_help_offset);
	Cvar_RegisterVariable (&vr_wpn_dir_weight_2h_help_mult);
	Cvar_RegisterVariable (&vr_gunmodelpitch);
	Cvar_RegisterVariable (&vr_hand_grips_weapon);
	Cvar_RegisterVariable (&vr_grip_vertex);
	Cvar_RegisterVariable (&vr_hand_scale);
	Cvar_RegisterVariable (&vr_gunangle);
	Cvar_RegisterVariable (&vr_gunyaw);
	Cvar_RegisterVariable (&vr_aimmode);
	Cvar_RegisterVariable (&vr_movement_mode);
	Cvar_RegisterVariable (&vr_roomscale_jump);
	Cvar_RegisterVariable (&vr_roomscale_jump_threshold);
	Cvar_RegisterVariable (&vr_roomscale_move_mult);
	Cvar_RegisterVariable (&vr_throw_algorithm);
	Cvar_RegisterVariable (&vr_throw_up_center_of_mass);
	Cvar_RegisterVariable (&vr_offhandpitch);
	Cvar_RegisterVariable (&vr_offhandyaw);
	Cvar_RegisterVariable (&vr_viewkick);
	Cvar_RegisterVariable (&vr_disablehaptics);
	Cvar_RegisterVariable (&vr_enable_joystick_turn);
	Cvar_RegisterVariable (&vr_player_shadows);
	Cvar_RegisterVariable (&vr_msaa);
	Cvar_RegisterVariable (&vr_novrinit);
	Cvar_RegisterVariable (&vr_fakevr_handroll);
	Cvar_RegisterVariable (&vr_hud_scale);
	Cvar_RegisterVariable (&vr_sbar_mode);
	Cvar_RegisterVariable (&vr_sbar_offset_x);
	Cvar_RegisterVariable (&vr_sbar_offset_y);
	Cvar_RegisterVariable (&vr_sbar_offset_z);
	Cvar_RegisterVariable (&vr_sbar_offset_pitch);
	Cvar_RegisterVariable (&vr_sbar_offset_yaw);
	Cvar_RegisterVariable (&vr_sbar_offset_roll);
	Cvar_RegisterVariable (&vr_menumode);
	Cvar_RegisterVariable (&vr_menu_mouse_pointer_hand);
	Cvar_RegisterVariable (&vr_crosshair);
	Cvar_RegisterVariable (&vr_crosshair_depth);
	Cvar_RegisterVariable (&vr_crosshair_size);
	Cvar_RegisterVariable (&vr_crosshair_alpha);
	Cvar_RegisterVariable (&vr_crosshairy);
	Cvar_RegisterVariable (&vr_spinreload_pitch_speed);
	Cvar_RegisterVariable (&vr_spinreload_x_angular_threshold);
	Cvar_RegisterVariable (&vr_autosave_seconds);
	Cvar_RegisterVariable (&vr_autosave_on_changelevel);
	Cvar_RegisterVariable (&vr_autosave_show_message);
	Cvar_RegisterVariable (&vr_show_hip_holsters);
	Cvar_RegisterVariable (&vr_show_shoulder_holsters);
	Cvar_RegisterVariable (&vr_show_upper_holsters);
	Cvar_RegisterVariable (&vr_show_weapon_text);
	Cvar_RegisterVariable (&vr_vrtorso_debuglines_enabled);
	Cvar_RegisterVariable (&vr_debug_print_handvel);
	Cvar_RegisterVariable (&vr_debug_print_headvel);
	Cvar_RegisterVariable (&vr_debug_show_hand_pos_and_rot);
	Cvar_RegisterVariable (&vr_activestartpaknameidx);
	Cvar_RegisterVariable (&vr_gunroll);
	Cvar_RegisterVariable (&vr_gun_z_offset);

	Cvar_RegisterVariable (&vr_shoulder_offset_x);
	Cvar_RegisterVariable (&vr_shoulder_offset_y);
	Cvar_RegisterVariable (&vr_shoulder_offset_z);
	Cvar_RegisterVariable (&vr_hip_offset_x);
	Cvar_RegisterVariable (&vr_hip_offset_y);
	Cvar_RegisterVariable (&vr_hip_offset_z);
	Cvar_RegisterVariable (&vr_hip_holster_thresh);
	Cvar_RegisterVariable (&vr_shoulder_holster_offset_x);
	Cvar_RegisterVariable (&vr_shoulder_holster_offset_y);
	Cvar_RegisterVariable (&vr_shoulder_holster_offset_z);
	Cvar_RegisterVariable (&vr_shoulder_holster_thresh);
	Cvar_RegisterVariable (&vr_upper_holster_offset_x);
	Cvar_RegisterVariable (&vr_upper_holster_offset_y);
	Cvar_RegisterVariable (&vr_upper_holster_offset_z);
	Cvar_RegisterVariable (&vr_upper_holster_thresh);

	// vkQuake ships gamma 0.9 / contrast 1.4, a brightened look tuned for a
	// monitor. quakevr runs both at 1 (gl_vidsdl.cpp:150-152); applied to a
	// headset, vkQuake's defaults lift the blacks and read as washed out.
	Cvar_SetQuick (&vid_gamma, "1");
	Cvar_SetQuick (&vid_contrast, "1");

	Cmd_AddCommand ("vr_calibrate", VR_XR_Calibrate_f);
	// The QuakeC binds all of these by name; see the block where they are
	// defined for why every one of them has to exist.
	Cvar_RegisterVariable (&vr_enabled);
	Cvar_RegisterVariable (&vr_fakevr);
	Cvar_RegisterVariable (&vr_forcegrab_mode);
	Cvar_RegisterVariable (&vr_forcegrab_powermult);
	Cvar_RegisterVariable (&vr_forcegrab_range);
	Cvar_RegisterVariable (&vr_forcegrab_radius);
	Cvar_RegisterVariable (&vr_forcegrab_eligible_particles);
	Cvar_RegisterVariable (&vr_forcegrab_eligible_haptics);
	Cvar_RegisterVariable (&vr_forcegrabbable_ammo_boxes);
	Cvar_RegisterVariable (&vr_forcegrabbable_health_boxes);
	Cvar_RegisterVariable (&vr_forcegrabbable_return_time_deathmatch);
	Cvar_RegisterVariable (&vr_forcegrabbable_return_time_singleplayer);
	Cvar_RegisterVariable (&vr_melee_dmg_multiplier);
	Cvar_RegisterVariable (&vr_melee_range_multiplier);
	Cvar_RegisterVariable (&vr_melee_bloodlust);
	Cvar_RegisterVariable (&vr_melee_bloodlust_mult);
	Cvar_RegisterVariable (&vr_headbutt_damage_mult);
	Cvar_RegisterVariable (&vr_headbutt_velocity_threshold);
	Cvar_RegisterVariable (&vr_enemy_drops);
	Cvar_RegisterVariable (&vr_enemy_drops_chance_mult);
	Cvar_RegisterVariable (&vr_ammobox_drops);
	Cvar_RegisterVariable (&vr_ammobox_drops_chance_mult);
	Cvar_RegisterVariable (&vr_positional_damage);
	Cvar_RegisterVariable (&vr_holster_mode);
	Cvar_RegisterVariable (&vr_holster_haptics);
	Cvar_RegisterVariable (&vr_reload_mode);
	Cvar_RegisterVariable (&vr_weapon_cycle_mode);
	Cvar_RegisterVariable (&vr_weapondrop_particles);
	Cvar_RegisterVariable (&vr_2h_spread_reduction);
	Cvar_RegisterVariable (&vr_2h_throw_velocity_mult);
	Cvar_RegisterVariable (&vr_verbosebots);

	Cmd_AddCommand ("vr_recenter", VR_XR_Recenter_f);
	Cmd_AddCommand ("vr_scaledump", VR_XR_ScaleDump_f);
	Cmd_AddCommand ("vr_bodydump", VR_XR_BodyDump_f);

	if (COM_CheckParm ("-novr") || !COM_CheckParm ("-vr"))
		return; // flat mode; say nothing

	Con_Printf ("\nOpenXR: initializing\n");

	// --- does a runtime exist, and does it speak Vulkan? ---
	res = xrEnumerateInstanceExtensionProperties (NULL, 0, &ext_count, NULL);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: no runtime available (%s)\n", XR_ResultStr (res));
		return;
	}

	ext_props = (XrExtensionProperties *)Mem_Alloc (sizeof (XrExtensionProperties) * ext_count);
	for (i = 0; i < ext_count; i++)
		ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;

	res = xrEnumerateInstanceExtensionProperties (NULL, ext_count, &ext_count, ext_props);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: could not enumerate extensions (%s)\n", XR_ResultStr (res));
		Mem_Free (ext_props);
		return;
	}

	if (!XR_HasExtension (ext_props, ext_count, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
	{
		Con_Warning ("OpenXR: runtime lacks %s\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		Mem_Free (ext_props);
		return;
	}
	// --- instance ---
	enabled_exts[num_enabled_exts++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;

	// Finger tracking is optional -- most controllers cannot report joints, and
	// the fingers fall back to being driven by the analog grip.
	// Checked before ext_props is released.
	if (XR_HasExtension (ext_props, ext_count, XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		enabled_exts[num_enabled_exts++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;

	Mem_Free (ext_props);

	memset (&instance_info, 0, sizeof (instance_info));
	instance_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	instance_info.enabledExtensionCount = num_enabled_exts;
	instance_info.enabledExtensionNames = enabled_exts;

	app = &instance_info.applicationInfo;
	q_strlcpy (app->applicationName, "vkQuake VR", XR_MAX_APPLICATION_NAME_SIZE);
	q_strlcpy (app->engineName, "vkQuake", XR_MAX_ENGINE_NAME_SIZE);
	app->applicationVersion = 1;
	app->engineVersion = 1;
	// request 1.0: 1.1 runtimes accept it, 1.0-only runtimes require it
	app->apiVersion = XR_API_VERSION_1_0;

	res = xrCreateInstance (&instance_info, &xr_instance);
	if (XR_FAILED (res))
	{
		xr_instance = XR_NULL_HANDLE; // so XR_ResultStr stays on the numeric path
		Con_Warning ("OpenXR: xrCreateInstance failed (%s)\n", XR_ResultStr (res));
		return;
	}

	memset (&instance_props, 0, sizeof (instance_props));
	instance_props.type = XR_TYPE_INSTANCE_PROPERTIES;
	if (XR_SUCCEEDED (xrGetInstanceProperties (xr_instance, &instance_props)))
	{
		Con_Printf (
			"OpenXR: runtime \"%s\" %u.%u.%u\n", instance_props.runtimeName, (unsigned)XR_VERSION_MAJOR (instance_props.runtimeVersion),
			(unsigned)XR_VERSION_MINOR (instance_props.runtimeVersion), (unsigned)XR_VERSION_PATCH (instance_props.runtimeVersion));
	}

	// --- headset ---
	memset (&system_info, 0, sizeof (system_info));
	system_info.type = XR_TYPE_SYSTEM_GET_INFO;
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	res = xrGetSystem (xr_instance, &system_info, &xr_system);
	if (XR_FAILED (res))
	{
		// XR_ERROR_FORM_FACTOR_UNAVAILABLE just means the headset is off/asleep
		Con_Warning ("OpenXR: no headset available (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return;
	}

	memset (&system_props, 0, sizeof (system_props));
	system_props.type = XR_TYPE_SYSTEM_PROPERTIES;
	if (XR_SUCCEEDED (xrGetSystemProperties (xr_instance, xr_system, &system_props)))
	{
		Con_Printf ("OpenXR: HMD \"%s\"\n", system_props.systemName);
		Con_Printf (
			"OpenXR: max swapchain %ux%u, %u layers\n", (unsigned)system_props.graphicsProperties.maxSwapchainImageWidth,
			(unsigned)system_props.graphicsProperties.maxSwapchainImageHeight, (unsigned)system_props.graphicsProperties.maxLayerCount);
		Con_Printf (
			"OpenXR: orientation %s, position %s\n", system_props.trackingProperties.orientationTracking ? "yes" : "no",
			system_props.trackingProperties.positionTracking ? "yes" : "no");
	}

	// --- per-eye render target size ---
	res = xrEnumerateViewConfigurationViews (xr_instance, xr_system, XR_VIEW_CONFIG, 0, &view_count, NULL);
	if (XR_FAILED (res) || view_count != 2)
	{
		Con_Warning ("OpenXR: stereo view configuration unavailable (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return;
	}

	for (i = 0; i < 2; i++)
	{
		memset (&views[i], 0, sizeof (views[i]));
		views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}

	res = xrEnumerateViewConfigurationViews (xr_instance, xr_system, XR_VIEW_CONFIG, 2, &view_count, views);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: could not query view configuration (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return;
	}

	vr_xr_eye_width = views[0].recommendedImageRectWidth;
	vr_xr_eye_height = views[0].recommendedImageRectHeight;
	Con_Printf ("OpenXR: per-eye render target %ux%u (%ux MSAA)\n", (unsigned)vr_xr_eye_width, (unsigned)vr_xr_eye_height, (unsigned)views[0].recommendedSwapchainSampleCount);

	// --- what the runtime needs from Vulkan ---
	// must be called before vkCreateInstance/vkCreateDevice, hence VR_XR_Init runs before VID_Init
	res = xrGetInstanceProcAddr (xr_instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&pfn_get_vk_reqs);
	if (XR_SUCCEEDED (res) && pfn_get_vk_reqs)
	{
		memset (&vk_reqs, 0, sizeof (vk_reqs));
		vk_reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
		if (XR_SUCCEEDED (pfn_get_vk_reqs (xr_instance, xr_system, &vk_reqs)))
		{
			Con_Printf (
				"OpenXR: needs Vulkan %u.%u.%u - %u.%u.%u\n", (unsigned)XR_VERSION_MAJOR (vk_reqs.minApiVersionSupported),
				(unsigned)XR_VERSION_MINOR (vk_reqs.minApiVersionSupported), (unsigned)XR_VERSION_PATCH (vk_reqs.minApiVersionSupported),
				(unsigned)XR_VERSION_MAJOR (vk_reqs.maxApiVersionSupported), (unsigned)XR_VERSION_MINOR (vk_reqs.maxApiVersionSupported),
				(unsigned)XR_VERSION_PATCH (vk_reqs.maxApiVersionSupported));
		}
	}

	vr_xr_active = true;
	Con_Printf ("OpenXR: ready\n\n");
}

/*
	VULKAN CREATION

	Under XR_KHR_vulkan_enable2 the runtime creates the Vulkan instance and
	device on our behalf, wrapping our own create-infos so it can splice in
	whatever extensions the compositor requires. It also chooses the physical
	device, which matters on multi-GPU machines: the headset is attached to
	one specific adapter and rendering on the other would mean a blit across
	the PCIe bus every frame, or simply fail.

================================================================================
*/

/*
XR_GetProc
===============
*/
static void *XR_GetProc (const char *name)
{
	PFN_xrVoidFunction fn = NULL;
	if (xr_instance == XR_NULL_HANDLE)
		return NULL;
	if (XR_FAILED (xrGetInstanceProcAddr (xr_instance, name, &fn)))
		return NULL;
	return (void *)fn;
}

/*
VR_XR_CreateVulkanInstance
===============
*/
qboolean VR_XR_CreateVulkanInstance (PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo *create_info, VkInstance *out_instance, VkResult *vk_err)
{
	PFN_xrCreateVulkanInstanceKHR pfn;
	XrVulkanInstanceCreateInfoKHR info;
	XrResult					  res;

	if (!vr_xr_active)
		return false;

	pfn = (PFN_xrCreateVulkanInstanceKHR)XR_GetProc ("xrCreateVulkanInstanceKHR");
	if (!pfn)
	{
		Con_Warning ("OpenXR: xrCreateVulkanInstanceKHR unavailable, falling back to flat\n");
		VR_XR_Shutdown ();
		return false;
	}

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	info.systemId = xr_system;
	info.pfnGetInstanceProcAddr = gipa;
	info.vulkanCreateInfo = create_info;
	info.vulkanAllocator = NULL;

	res = pfn (xr_instance, &info, out_instance, vk_err);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: xrCreateVulkanInstanceKHR failed (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return false;
	}

	return true;
}

/*
VR_XR_GetVulkanPhysicalDevice
===============
*/
qboolean VR_XR_GetVulkanPhysicalDevice (PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice *out_device)
{
	PFN_xrGetVulkanGraphicsDevice2KHR pfn;
	XrVulkanGraphicsDeviceGetInfoKHR  info;
	XrResult						  res;

	(void)gipa;

	if (!vr_xr_active)
		return false;

	pfn = (PFN_xrGetVulkanGraphicsDevice2KHR)XR_GetProc ("xrGetVulkanGraphicsDevice2KHR");
	if (!pfn)
	{
		Con_Warning ("OpenXR: xrGetVulkanGraphicsDevice2KHR unavailable, falling back to flat\n");
		VR_XR_Shutdown ();
		return false;
	}

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = xr_system;
	info.vulkanInstance = instance;

	res = pfn (xr_instance, &info, out_device);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: could not resolve the headset's GPU (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return false;
	}

	return true;
}

/*
VR_XR_CreateVulkanDevice
===============
*/
qboolean VR_XR_CreateVulkanDevice (
	PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info, VkDevice *out_device, VkResult *vk_err)
{
	PFN_xrCreateVulkanDeviceKHR pfn;
	XrVulkanDeviceCreateInfoKHR info;
	XrResult					res;

	if (!vr_xr_active)
		return false;

	pfn = (PFN_xrCreateVulkanDeviceKHR)XR_GetProc ("xrCreateVulkanDeviceKHR");
	if (!pfn)
	{
		Con_Warning ("OpenXR: xrCreateVulkanDeviceKHR unavailable, falling back to flat\n");
		VR_XR_Shutdown ();
		return false;
	}

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	info.systemId = xr_system;
	info.pfnGetInstanceProcAddr = gipa;
	info.vulkanPhysicalDevice = physical_device;
	info.vulkanCreateInfo = create_info;
	info.vulkanAllocator = NULL;

	res = pfn (xr_instance, &info, out_device, vk_err);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: xrCreateVulkanDeviceKHR failed (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return false;
	}

	return true;
}

/*
	SESSION

	An OpenXR session is a state machine, not just a handle. The runtime tells
	us when it is ready for us to begin, when we are actually visible, and when
	to stop; submitting frames outside the running states is an error. Virtual
	Desktop in particular parks the session while the headset is off the head,
	so this has to be driven properly rather than assumed.

================================================================================
*/

static XrSession	  xr_session = XR_NULL_HANDLE;
static XrSpace		  xr_space = XR_NULL_HANDLE;
// The head, as its own reference space. Located each frame purely for its
// velocity, which room-scale jumping needs and xrLocateViews does not report.
static XrSpace		  xr_view_space = XR_NULL_HANDLE;
static vec3_t		  xr_head_velocity;
static qboolean		  xr_head_vel_valid = false;
static XrSessionState xr_state = XR_SESSION_STATE_UNKNOWN;
static qboolean		  xr_session_running = false;

static XrSwapchain				  xr_swapchain[VR_XR_EYES] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static uint32_t					  xr_swapchain_len[VR_XR_EYES] = {0, 0};
static XrSwapchainImageVulkanKHR *xr_swapchain_images[VR_XR_EYES] = {NULL, NULL};
static int64_t					  xr_swapchain_format = 0;

static XrFrameState xr_frame_state;
static qboolean		xr_frame_begun = false;

// filled by xrLocateViews each frame; drives the projection layer
static XrView	xr_views[VR_XR_EYES];
static qboolean xr_views_valid = false;

// index of the swapchain image currently acquired per eye, and whether we hold it
static uint32_t xr_acquired_index[VR_XR_EYES] = {0, 0};
static qboolean xr_eye_acquired[VR_XR_EYES] = {false, false};

// which eyes actually received an image this frame; the projection layer is
// only submitted once both have, otherwise the runtime would composite a stale
// or uninitialised image for the missing eye
static qboolean xr_eye_rendered[VR_XR_EYES] = {false, false};

/*
XR_DestroySession

Tears down session-scoped objects. Safe to call repeatedly and safe to call
when nothing was ever created, which is what makes the failure paths in
VR_XR_CreateSession able to just bail to VR_XR_Shutdown.
===============
*/
static void XR_DestroySession (void)
{
	uint32_t eye;

	for (eye = 0; eye < VR_XR_EYES; eye++)
	{
		if (xr_swapchain[eye] != XR_NULL_HANDLE)
		{
			xrDestroySwapchain (xr_swapchain[eye]);
			xr_swapchain[eye] = XR_NULL_HANDLE;
		}
		if (xr_swapchain_images[eye])
		{
			Mem_Free (xr_swapchain_images[eye]);
			xr_swapchain_images[eye] = NULL;
		}
		xr_swapchain_len[eye] = 0;
	}

	if (xr_space != XR_NULL_HANDLE)
	{
		xrDestroySpace (xr_space);
		xr_space = XR_NULL_HANDLE;
	}

	if (xr_session != XR_NULL_HANDLE)
	{
		if (xr_session_running)
		{
			xrEndSession (xr_session);
			xr_session_running = false;
		}
		xrDestroySession (xr_session);
		xr_session = XR_NULL_HANDLE;
	}

	xr_state = XR_SESSION_STATE_UNKNOWN;
	xr_frame_begun = false;
}

/*
XR_ChooseSwapchainFormat

The runtime publishes the formats it can composite. Prefer UNORM, and prefer it
in the same order the window swapchain uses.

vkCmdBlitImage converts between formats, and the frame we copy has already been
through vkQuake's postprocess -- it is display-ready, not linear. Copying it
into an sRGB target makes the blit re-encode it a second time, which lifts the
blacks and washes the whole image out. Matching UNORM keeps the copy a copy.
===============
*/
static int64_t XR_ChooseSwapchainFormat (void)
{
	static const int64_t preferred[] = {
		VK_FORMAT_B8G8R8A8_UNORM, // what the window swapchain picks first
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_SRGB,
		VK_FORMAT_R8G8B8A8_SRGB,
	};
	uint32_t count = 0;
	int64_t *formats;
	int64_t	 chosen = 0;
	uint32_t i, j;

	if (XR_FAILED (xrEnumerateSwapchainFormats (xr_session, 0, &count, NULL)) || count == 0)
		return 0;

	formats = (int64_t *)Mem_Alloc (sizeof (int64_t) * count);
	if (XR_FAILED (xrEnumerateSwapchainFormats (xr_session, count, &count, formats)))
	{
		Mem_Free (formats);
		return 0;
	}

	for (i = 0; i < countof (preferred) && !chosen; i++)
	{
		for (j = 0; j < count; j++)
		{
			if (formats[j] == preferred[i])
			{
				chosen = formats[j];
				break;
			}
		}
	}

	if (!chosen)
		chosen = formats[0]; // the runtime lists its own preference first

	Mem_Free (formats);
	return chosen;
}

/*
VR_XR_CreateSession
===============
*/
void VR_XR_CreateSession (VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family_index, uint32_t queue_index)
{
	XrGraphicsBindingVulkan2KHR binding;
	XrSessionCreateInfo			session_info;
	XrReferenceSpaceCreateInfo	space_info;
	XrSwapchainCreateInfo		swapchain_info;
	XrResult					res;
	uint32_t					eye, i;

	if (!vr_xr_active)
		return;

	memset (&binding, 0, sizeof (binding));
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
	binding.instance = instance;
	binding.physicalDevice = physical_device;
	binding.device = device;
	binding.queueFamilyIndex = queue_family_index;
	binding.queueIndex = queue_index;

	memset (&session_info, 0, sizeof (session_info));
	session_info.type = XR_TYPE_SESSION_CREATE_INFO;
	session_info.next = &binding;
	session_info.systemId = xr_system;

	res = xrCreateSession (xr_instance, &session_info, &xr_session);
	if (XR_FAILED (res))
	{
		Con_Warning ("OpenXR: xrCreateSession failed (%s)\n", XR_ResultStr (res));
		VR_XR_Shutdown ();
		return;
	}

	// STAGE is room-scale with a floor-level origin, which is what roomscale
	// movement wants. Not every runtime offers it; LOCAL is the fallback.
	memset (&space_info, 0, sizeof (space_info));
	space_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;

	res = xrCreateReferenceSpace (xr_session, &space_info, &xr_space);
	if (XR_FAILED (res))
	{
		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		res = xrCreateReferenceSpace (xr_session, &space_info, &xr_space);
		if (XR_FAILED (res))
		{
			Con_Warning ("OpenXR: no usable reference space (%s)\n", XR_ResultStr (res));
			VR_XR_Shutdown ();
			return;
		}
		Con_Printf ("OpenXR: using LOCAL space (no room-scale stage)\n");
	}



	// A VIEW-type reference space, so the head can be located with velocity.
	// Failure is not fatal: it only costs room-scale jumping.
	{
		XrReferenceSpaceCreateInfo view_info;
		memset (&view_info, 0, sizeof (view_info));
		view_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
		view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		view_info.poseInReferenceSpace.orientation.w = 1.0f;
		if (XR_FAILED (xrCreateReferenceSpace (xr_session, &view_info, &xr_view_space)))
			xr_view_space = XR_NULL_HANDLE;
	}

	xr_swapchain_format = XR_ChooseSwapchainFormat ();
	if (!xr_swapchain_format)
	{
		Con_Warning ("OpenXR: no usable swapchain format\n");
		VR_XR_Shutdown ();
		return;
	}

	for (eye = 0; eye < VR_XR_EYES; eye++)
	{
		memset (&swapchain_info, 0, sizeof (swapchain_info));
		swapchain_info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		swapchain_info.format = xr_swapchain_format;
		swapchain_info.sampleCount = 1;
		swapchain_info.width = vr_xr_eye_width;
		swapchain_info.height = vr_xr_eye_height;
		swapchain_info.faceCount = 1;
		swapchain_info.arraySize = 1;
		swapchain_info.mipCount = 1;

		res = xrCreateSwapchain (xr_session, &swapchain_info, &xr_swapchain[eye]);
		if (XR_FAILED (res))
		{
			Con_Warning ("OpenXR: xrCreateSwapchain failed for eye %u (%s)\n", (unsigned)eye, XR_ResultStr (res));
			VR_XR_Shutdown ();
			return;
		}

		if (XR_FAILED (xrEnumerateSwapchainImages (xr_swapchain[eye], 0, &xr_swapchain_len[eye], NULL)))
		{
			Con_Warning ("OpenXR: could not count swapchain images\n");
			VR_XR_Shutdown ();
			return;
		}

		xr_swapchain_images[eye] = (XrSwapchainImageVulkanKHR *)Mem_Alloc (sizeof (XrSwapchainImageVulkanKHR) * xr_swapchain_len[eye]);
		for (i = 0; i < xr_swapchain_len[eye]; i++)
			xr_swapchain_images[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;

		if (XR_FAILED (xrEnumerateSwapchainImages (
				xr_swapchain[eye], xr_swapchain_len[eye], &xr_swapchain_len[eye], (XrSwapchainImageBaseHeader *)xr_swapchain_images[eye])))
		{
			Con_Warning ("OpenXR: could not enumerate swapchain images\n");
			VR_XR_Shutdown ();
			return;
		}
	}

	Con_Printf (
		"OpenXR: session created, %ux%u per eye, %u images, format %d\n", (unsigned)vr_xr_eye_width, (unsigned)vr_xr_eye_height,
		(unsigned)xr_swapchain_len[0], (int)xr_swapchain_format);

	VR_XR_InitInput ();
}

/*
VR_XR_SessionRunning
===============
*/
qboolean VR_XR_SessionRunning (void)
{
	const qboolean running = vr_xr_active && xr_session_running;

	// Mirror the real state into the cvar the QuakeC reads. The QC tests
	// vr_enabled to decide whether it is in VR at all, so it gates nearly every
	// VR behaviour in there; it is driven from here rather than being set once
	// at startup so that taking the headset off is reflected honestly.
	if ((vr_enabled.value != 0.0f) != (running != 0))
		Cvar_SetValueQuick (&vr_enabled, running ? 1.0f : 0.0f);

	return running;
}

/*
VR_XR_PumpEvents

Drives the session state machine. Must be called regularly or the runtime will
consider us unresponsive.
===============
*/
void VR_XR_PumpEvents (void)
{
	XrEventDataBuffer ev;

	if (!vr_xr_active || xr_session == XR_NULL_HANDLE)
		return;

	for (;;)
	{
		memset (&ev, 0, sizeof (ev));
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		if (xrPollEvent (xr_instance, &ev) != XR_SUCCESS)
			break;

		switch (ev.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&ev;
			xr_state = changed->state;

			if (xr_state == XR_SESSION_STATE_READY && !xr_session_running)
			{
				XrSessionBeginInfo begin_info;
				memset (&begin_info, 0, sizeof (begin_info));
				begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
				begin_info.primaryViewConfigurationType = XR_VIEW_CONFIG;
				if (XR_SUCCEEDED (xrBeginSession (xr_session, &begin_info)))
				{
					xr_session_running = true;
					Con_Printf ("OpenXR: session running\n");
					// A map may have loaded while the session was idle, so
					// re-apply the model offsets now rather than waiting for
					// the next map load.
					VR_XR_ModAllModels ();
				}
			}
			else if (xr_state == XR_SESSION_STATE_STOPPING && xr_session_running)
			{
				xrEndSession (xr_session);
				xr_session_running = false;
				Con_Printf ("OpenXR: session stopped\n");
			}
			else if (xr_state == XR_SESSION_STATE_EXITING || xr_state == XR_SESSION_STATE_LOSS_PENDING)
			{
				Con_Printf ("OpenXR: session exiting\n");
				xr_session_running = false;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			Con_Warning ("OpenXR: runtime is going away\n");
			xr_session_running = false;
			break;
		default:
			break;
		}
	}
}

/*
VR_XR_BeginFrame

Returns false when this frame should not be rendered. The runtime can legally
ask us to idle -- e.g. headset off the head -- and burning GPU then is waste.
===============
*/
qboolean VR_XR_BeginFrame (void)
{
	XrFrameWaitInfo	 wait_info;
	XrFrameBeginInfo begin_info;

	if (!VR_XR_SessionRunning ())
		return false;

	memset (&wait_info, 0, sizeof (wait_info));
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	memset (&xr_frame_state, 0, sizeof (xr_frame_state));
	xr_frame_state.type = XR_TYPE_FRAME_STATE;

	// blocks until the runtime wants the next frame: this is the pacing
	// mechanism, and is why VR framerate is driven by the compositor, not us
	if (XR_FAILED (xrWaitFrame (xr_session, &wait_info, &xr_frame_state)))
		return false;

	memset (&begin_info, 0, sizeof (begin_info));
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	if (XR_FAILED (xrBeginFrame (xr_session, &begin_info)))
		return false;

	xr_frame_begun = true;

	// controller state, sampled against this frame's predicted display time
	VR_XR_SyncInput ();

	// Acquire both eye images here, on the main thread. The blit itself is
	// recorded from GL_EndRenderingTask, which vkQuake may run on a task
	// worker -- recording Vulkan commands there is fine, but calling into the
	// OpenXR runtime from it while xrEndFrame runs on the main thread is not.
	// Keeping every xr* call on this thread is what lets r_tasks stay enabled.
	XR_AcquireEyes ();

	// where the eyes will be at predicted display time -- not where they are
	// now. Using anything else is what makes VR feel laggy.
	xr_views_valid = false;
	if (xr_frame_state.shouldRender)
	{
		XrViewLocateInfo locate_info;
		XrViewState		 view_state;
		uint32_t		 count = 0;
		uint32_t		 i;

		memset (&locate_info, 0, sizeof (locate_info));
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIG;
		locate_info.displayTime = xr_frame_state.predictedDisplayTime;
		locate_info.space = xr_space;

		memset (&view_state, 0, sizeof (view_state));
		view_state.type = XR_TYPE_VIEW_STATE;

		for (i = 0; i < VR_XR_EYES; i++)
		{
			memset (&xr_views[i], 0, sizeof (xr_views[i]));
			xr_views[i].type = XR_TYPE_VIEW;
		}

		if (XR_SUCCEEDED (xrLocateViews (xr_session, &locate_info, &view_state, VR_XR_EYES, &count, xr_views)) && count == VR_XR_EYES &&
			(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		{
			xr_views_valid = true;
		}
	}

	// needs the fresh view poses, so it has to follow xrLocateViews
	XR_UpdateRoomscale ();

	return xr_frame_state.shouldRender ? true : false;
}

/*
XR_AcquireEyes

Main thread only. Grabs an image from each eye swapchain and blocks until the
runtime says it is safe to write to.
===============
*/
static void XR_AcquireEyes (void)
{
	XrSwapchainImageAcquireInfo acquire_info;
	XrSwapchainImageWaitInfo	wait_info;
	uint32_t					eye;

	for (eye = 0; eye < VR_XR_EYES; eye++)
	{
		if (xr_eye_acquired[eye] || xr_swapchain[eye] == XR_NULL_HANDLE)
			continue;

		memset (&acquire_info, 0, sizeof (acquire_info));
		acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		if (XR_FAILED (xrAcquireSwapchainImage (xr_swapchain[eye], &acquire_info, &xr_acquired_index[eye])))
			continue;

		memset (&wait_info, 0, sizeof (wait_info));
		wait_info.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		wait_info.timeout = XR_INFINITE_DURATION;
		if (XR_FAILED (xrWaitSwapchainImage (xr_swapchain[eye], &wait_info)))
			continue;

		xr_eye_acquired[eye] = true;
	}
}

/*
VR_XR_BlitToEyes

Records Vulkan commands only -- no OpenXR calls -- so it is safe to run from a
task worker thread.
===============
*/
qboolean VR_XR_BlitToEye (VkCommandBuffer cb, VkImage src, uint32_t src_width, uint32_t src_height, int which_eye)
{
	const uint32_t		 eye = (uint32_t)which_eye;
	uint32_t			 eye_it;
	VkImageMemoryBarrier barrier;
	VkImageBlit			 blit;

	// Diagnostic: say exactly which precondition rejected the blit. Guessing at
	// this from the symptom in the headset has not been productive.
	if (VR_XR_SessionRunning ()) // pre-session frames are expected to reject; ignore them
	{
		static int reported = 0;
		int		   reason = 0;
		if (!xr_frame_begun)
			reason = 2;
		else if (!xr_views_valid)
			reason = 3;
		else if (src == VK_NULL_HANDLE)
			reason = 4;
		else if (which_eye < 0 || which_eye >= VR_XR_EYES)
			reason = 5;
		else if (!xr_eye_acquired[eye])
			reason = 6;
		if (reason && reported < 5)
		{
			reported++;
			Con_Printf ("XR blit rejected: reason %d (eye=%d)\n", reason, which_eye);
		}
	}

	if (!VR_XR_SessionRunning () || !xr_frame_begun || !xr_views_valid || src == VK_NULL_HANDLE)
		return false;
	if (which_eye < 0 || which_eye >= VR_XR_EYES || !xr_eye_acquired[eye])
		return false;

	// STEREO: this render belongs to exactly one eye.
	for (eye_it = eye; eye_it == eye; eye_it++)
	{
		VkImage dst = xr_swapchain_images[eye_it][xr_acquired_index[eye_it]].image;

		// the runtime hands the image back in an undefined layout
		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = dst;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		// source is the window swapchain image, left in PRESENT_SRC by the main pass
		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = src;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		// scales the window-sized frame up to the eye target; linear filter
		// because those two resolutions have no reason to match
		memset (&blit, 0, sizeof (blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)src_width;
		blit.srcOffsets[1].y = (int32_t)src_height;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1].x = (int32_t)vr_xr_eye_width;
		blit.dstOffsets[1].y = (int32_t)vr_xr_eye_height;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage (
			cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		// hand the eye image to the compositor, and put the window image back
		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = dst;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = src;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		xr_eye_rendered[eye_it] = true;
	}

	return true;
}

/*
	STEREO

	Two coordinate systems have to be reconciled. OpenXR is right-handed metres
	with X right, Y up, -Z forward. Quake is X forward, Y left, Z up, in units
	where a player is 56 tall for roughly 1.75m of human -- hence worldscale.

		quake_x =  -xr_z     (forward)
		quake_y =  -xr_x     (left)
		quake_z =   xr_y     (up)

	Rather than rebuild Quake's view matrix from the pose, the head orientation
	is converted into pitch/yaw/roll and fed through the renderer's existing
	path. That path is already correct and well tested; duplicating it in a new
	coordinate convention would only add somewhere new to be subtly wrong.

================================================================================
*/

int vr_xr_current_eye = -1;

// Values lifted from quakevr, which had them tuned against real play.
// A Quake unit is 1.5 inches, so world scale 1.0 works out to ~26.25 units per
// metre -- expressing it as a multiplier rather than a raw unit count means 1.0
// is "life size" and players can reason about it.
cvar_t vr_world_scale = {"vr_world_scale", "1.25", CVAR_ARCHIVE};

// The runtime reports head position relative to the floor, but Quake's player
// origin sits at the middle of the bounding box, not the feet. quakevr settled
// on -16 units to reconcile the two. (quakevr vr_cvars.cpp:60)
cvar_t vr_floor_offset = {"vr_floor_offset", "-21", CVAR_ARCHIVE};

#define VR_METERS_TO_UNITS (vr_world_scale.value / (1.5f * 0.0254f))

/*
XR_QuatToQuakeAngles

Derives Quake pitch/yaw/roll from an OpenXR orientation. Note Quake's pitch is
inverted: positive pitch looks *down*.
===============
*/
static void XR_QuatToQuakeAngles (const XrQuaternionf *q, float out_angles[3])
{
	float fwd[3], up[3];
	float qx = q->x, qy = q->y, qz = q->z, qw = q->w;
	float xr_fwd[3], xr_up[3];

	// forward is -Z and up is +Y in OpenXR, rotated by the orientation
	xr_fwd[0] = -2.0f * (qx * qz + qw * qy);
	xr_fwd[1] = -2.0f * (qy * qz - qw * qx);
	xr_fwd[2] = -(1.0f - 2.0f * (qx * qx + qy * qy));

	xr_up[0] = 2.0f * (qx * qy - qw * qz);
	xr_up[1] = 1.0f - 2.0f * (qx * qx + qz * qz);
	xr_up[2] = 2.0f * (qy * qz + qw * qx);

	// into Quake axes
	fwd[0] = -xr_fwd[2];
	fwd[1] = -xr_fwd[0];
	fwd[2] = xr_fwd[1];

	up[0] = -xr_up[2];
	up[1] = -xr_up[0];
	up[2] = xr_up[1];

	out_angles[1] = RAD2DEG (atan2f (fwd[1], fwd[0]));				   // yaw
	out_angles[0] = -RAD2DEG (asinf (CLAMP (-1.0f, fwd[2], 1.0f))); // pitch, inverted

	{
		// Roll is the angle of the head's up vector about the forward axis, so
		// it has to be measured against a roll-free frame built for THIS
		// forward -- not against world up.
		//
		// Using world up (up[2]) works only while looking level. Pitch the head
		// down and the roll-free up swings toward horizontal, so its world-Z
		// component collapses to zero, and atan2 against a vanishing reference
		// swings wildly: the whole view appears to skew left and right as you
		// look down.
		//
		// left is the horizontal left for this yaw, and cross(fwd, left) is the
		// up that goes with it, upright at level pitch and horizontal when
		// looking straight down. Roll then falls out cleanly at any pitch.
		//
		// atan2 rather than asin for the reason quakevr gives (vr.cpp:404-405):
		// asin saturates at +-90 and is symmetric, so a 120-degree roll would
		// report 60 and rolling further would run backwards.
		const float sy = sinf (DEG2RAD (out_angles[1]));
		const float cy = cosf (DEG2RAD (out_angles[1]));
		const float left[3] = {-sy, cy, 0.0f};
		float		roll_free_up[3];

		CrossProduct (fwd, left, roll_free_up);

		out_angles[2] = -RAD2DEG (atan2f (DotProduct (up, left), DotProduct (up, roll_free_up)));
	}
}

/*
VR_XR_EyePose
===============
*/
qboolean VR_XR_EyePose (float out_angles[3], float out_offset[3])
{
	const XrPosef *pose;
	float		   scale;

	if (vr_xr_current_eye < 0 || vr_xr_current_eye >= VR_XR_EYES || !xr_views_valid)
		return false;

	pose = &xr_views[vr_xr_current_eye].pose;
	scale = VR_METERS_TO_UNITS;

	XR_QuatToQuakeAngles (&pose->orientation, out_angles);

	// metres -> Quake units, XR axes -> Quake axes (quakevr vr.cpp:3511)
	out_offset[0] = -pose->position.z * scale;
	out_offset[1] = -pose->position.x * scale;
	out_offset[2] = pose->position.y * scale;

	// Horizontal head position is deliberately discarded, leaving only the
	// per-eye (IPD) offset. Physical walking reaches the world by moving the
	// player entity through room-scale, so letting it move the camera as well
	// would double-count it and let the view pass through walls.
	//
	// quakevr does this bluntly -- headPos.v[0] = 0; headPos.v[2] = 0; with the
	// comment "these two lines are what keep the head position stable (attached
	// to the player, instead of to the hmd)" (vr.cpp:2696-2697).
	//
	// It also retires the old consumed-movement accumulator, which grew without
	// bound whenever the server clipped a requested move -- against a wall, on
	// a ledge, when stuck -- and slid the whole rig away from the player.
	out_offset[0] -= xr_last_head_pos[0];
	out_offset[1] -= xr_last_head_pos[1];

	// floor-relative height -> Quake's centre-of-box origin (quakevr vr.cpp:3516)
	out_offset[2] += vr_floor_offset.value;

	return true;
}

/*
VR_XR_EyeProjectionMatrix

Matches the renderer's conventions: column major, reverse Z, Y flipped for
Vulkan clip space. Only the horizontal/vertical extents differ from the stock
frustum, and they are asymmetric.
===============
*/
qboolean VR_XR_EyeProjectionMatrix (float matrix[16], float farclip)
{
	const XrFovf *fov;
	float		  tan_left, tan_right, tan_up, tan_down;
	float		  tan_width, tan_height;
	float		  n, f;

	if (vr_xr_current_eye < 0 || vr_xr_current_eye >= VR_XR_EYES || !xr_views_valid)
		return false;

	fov = &xr_views[vr_xr_current_eye].fov;
	tan_left = tanf (fov->angleLeft);
	tan_right = tanf (fov->angleRight);
	tan_up = tanf (fov->angleUp);
	tan_down = tanf (fov->angleDown);

	tan_width = tan_right - tan_left;
	tan_height = tan_up - tan_down;
	if (tan_width == 0.0f || tan_height == 0.0f)
		return false;

	// a fixed near plane: the head can get arbitrarily close to geometry, and
	// the stock fov-derived near distance has no meaning for an HMD frustum
	n = 4.0f;
	f = farclip > n ? farclip : n + 1.0f;

	memset (matrix, 0, 16 * sizeof (float));

	matrix[0 * 4 + 0] = 2.0f / tan_width;
	matrix[2 * 4 + 0] = (tan_right + tan_left) / tan_width;

	matrix[1 * 4 + 1] = -2.0f / tan_height;
	matrix[2 * 4 + 1] = -(tan_up + tan_down) / tan_height;

	matrix[2 * 4 + 2] = f / (f - n) - 1.0f;
	matrix[2 * 4 + 3] = -1.0f;

	matrix[3 * 4 + 2] = (n * f) / (f - n);

	return true;
}

/*
VR_XR_ReleaseEyes
===============
*/
void VR_XR_ReleaseEyes (void)
{
	XrSwapchainImageReleaseInfo release_info;
	uint32_t					eye;

	for (eye = 0; eye < VR_XR_EYES; eye++)
	{
		if (!xr_eye_acquired[eye])
			continue;
		xr_eye_acquired[eye] = false;

		memset (&release_info, 0, sizeof (release_info));
		release_info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		xrReleaseSwapchainImage (xr_swapchain[eye], &release_info);
	}
}

/*
VR_XR_EndFrame

Submits no composition layers yet -- enough to prove the runtime accepts our
frame loop. Layers arrive with the stereo render path.
===============
*/
void VR_XR_EndFrame (void)
{
	XrFrameEndInfo end_info;

	if (!xr_frame_begun)
		return;
	// NB: xr_frame_begun stays true across the join below. The blit is deferred
	// to a task worker and does not actually execute until
	// GL_SynchronizeEndRenderingTask, so clearing the flag here would make
	// every blit reject itself for being outside a frame.

	XrCompositionLayerProjectionView views[VR_XR_EYES];
	XrCompositionLayerProjection	 layer;
	const XrCompositionLayerBaseHeader *layers[1];
	uint32_t						 eye;

	// Wait for the deferred end-rendering task FIRST. The blits are recorded on
	// a worker and set xr_eye_rendered there, so testing those flags before
	// joining would usually see them still clear -- the layer would be dropped
	// and the headset would show black, with the odd frame slipping through
	// whenever the worker happened to win the race.
	GL_SynchronizeEndRenderingTask ();
	xr_frame_begun = false; // safe now: every deferred blit has run

	memset (&end_info, 0, sizeof (end_info));
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = xr_frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = 0;
	end_info.layers = NULL;

	// Diagnostic: report how often each eye actually gets an image and how often
	// a layer is submitted. A flashing headset means layers are being dropped,
	// and this says which eye is missing rather than leaving it to guesswork.
	{
		static int frames = 0, e0 = 0, e1 = 0, submitted = 0, noviews = 0, shouldrender = 0;
		frames++;
		if (xr_eye_rendered[0])
			e0++;
		if (xr_eye_rendered[1])
			e1++;
		if (!xr_views_valid)
			noviews++;
		// the runtime tells us not to render while the headset is off the head;
		// zero eyes with shouldRender also zero is correct behaviour, not a bug
		if (xr_frame_state.shouldRender)
			shouldrender++;
		if (xr_eye_rendered[0] && xr_eye_rendered[1] && xr_views_valid)
			submitted++;
		if ((frames % 90) == 0)
		{
			Con_Printf (
				"XR: %d frames | shouldRender %d | eye0 %d | eye1 %d | layer %d | noviews %d\n", frames, shouldrender, e0, e1, submitted, noviews);
			Con_Printf (
				"XR hands: L trk%d pos(%.0f %.0f %.0f) trg%.2f grp%.2f stk(%.2f %.2f) | R trk%d pos(%.0f %.0f %.0f) trg%.2f grp%.2f stk(%.2f %.2f)\n",
				vr_xr_hand[0].tracked, vr_xr_hand[0].pos[0], vr_xr_hand[0].pos[1], vr_xr_hand[0].pos[2], vr_xr_hand[0].trigger, vr_xr_hand[0].grip,
				vr_xr_hand[0].stick[0], vr_xr_hand[0].stick[1], vr_xr_hand[1].tracked, vr_xr_hand[1].pos[0], vr_xr_hand[1].pos[1], vr_xr_hand[1].pos[2],
				vr_xr_hand[1].trigger, vr_xr_hand[1].grip, vr_xr_hand[1].stick[0], vr_xr_hand[1].stick[1]);

			// body placement, so torso and holster positions can be read off a
			// running session instead of reasoned about
			if (cl.viewentity > 0 && cl.viewentity < cl.max_edicts && cl.entities)
			{
				const float pz = cl.entities[cl.viewentity].origin[2];
				vec3_t		lh, rh, lu, ru, ls;
				float		t0 = 0.0f, t1 = 0.0f;

				if (cl.vrtorso.model && cl.vrtorso.model->type == mod_alias)
				{
					aliashdr_t *th = (aliashdr_t *)Mod_Extradata (cl.vrtorso.model);
					if (th)
					{
						t0 = cl.vrtorso.origin[2] - pz + th->scale_origin[2];
						t1 = t0 + th->scale[2] * 255.0f;
					}
				}

				VR_XR_HolsterSpot (QVR_HS_LEFT_HIP_HOLSTER, lh);
				VR_XR_HolsterSpot (QVR_HS_RIGHT_HIP_HOLSTER, rh);
				VR_XR_HolsterSpot (QVR_HS_LEFT_UPPER_HOLSTER, lu);
				VR_XR_HolsterSpot (QVR_HS_RIGHT_UPPER_HOLSTER, ru);
				VR_XR_HolsterSpot (QVR_HS_LEFT_SHOULDER_HOLSTER, ls);

				Con_Printf (
					"XR body: scale %.1f u/m | torso z %.1f spans %.1f..%.1f | Lhip(%.0f %.0f %.0f) Lupr(%.0f %.0f %.0f) Lshl(%.0f %.0f %.0f) | hip-upr %.1f\n",
					VR_METERS_TO_UNITS, cl.vrtorso.origin[2] - pz, t0, t1, lh[0] - cl.entities[cl.viewentity].origin[0],
					lh[1] - cl.entities[cl.viewentity].origin[1], lh[2] - pz, lu[0] - cl.entities[cl.viewentity].origin[0],
					lu[1] - cl.entities[cl.viewentity].origin[1], lu[2] - pz, ls[0] - cl.entities[cl.viewentity].origin[0],
					ls[1] - cl.entities[cl.viewentity].origin[1], ls[2] - pz,
					sqrtf (
						(lh[0] - lu[0]) * (lh[0] - lu[0]) + (lh[1] - lu[1]) * (lh[1] - lu[1]) + (lh[2] - lu[2]) * (lh[2] - lu[2])));
			}

			// The whole weapon position chain, end to end, so a report of the
			// gun sitting wrong can be attributed instead of guessed at.
			// wpn-head is the number that matters: it should equal the real
			// distance from your headset to your controller. If it does and
			// the gun still looks wrong, the error is downstream in the model
			// offsets, not here. Units are Quake units, 26.25 to the metre.
			if (cl.viewentity > 0 && cl.viewentity < cl.max_edicts && cl.entities)
			{
				const int	  mh = VR_XR_MainHand ();
				const vec3_t *po = &cl.entities[cl.viewentity].origin;
				vec3_t		  wo, wa;

				if (VR_XR_WeaponPose (*po, wo, wa))
					Con_Printf (
						"XR wpn: player(%.0f %.0f %.0f) head(%.0f %.0f %.0f) aim(%.0f %.0f %.0f) -> wpn(%.0f %.0f %.0f) | wpn-head(%.0f %.0f %.0f) "
						"ang(%.0f %.0f %.0f)\n",
						(*po)[0], (*po)[1], (*po)[2], xr_last_head_pos[0], xr_last_head_pos[1], xr_last_head_pos[2], vr_xr_hand[mh].aim_pos[0],
						vr_xr_hand[mh].aim_pos[1], vr_xr_hand[mh].aim_pos[2], wo[0], wo[1], wo[2], wo[0] - ((*po)[0] + xr_last_head_pos[0]),
						wo[1] - ((*po)[1] + xr_last_head_pos[1]), wo[2] - ((*po)[2] + xr_last_head_pos[2]), wa[0], wa[1], wa[2]);
			}
			frames = e0 = e1 = submitted = noviews = shouldrender = 0;
		}
	}

	// only present a layer when both eyes actually received an image this frame;
	// otherwise submit an empty frame, which is legal and keeps pacing
	if (xr_eye_rendered[0] && xr_eye_rendered[1] && xr_views_valid)
	{
		for (eye = 0; eye < VR_XR_EYES; eye++)
		{
			memset (&views[eye], 0, sizeof (views[eye]));
			views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			views[eye].pose = xr_views[eye].pose;
			views[eye].fov = xr_views[eye].fov;
			views[eye].subImage.swapchain = xr_swapchain[eye];
			views[eye].subImage.imageRect.offset.x = 0;
			views[eye].subImage.imageRect.offset.y = 0;
			views[eye].subImage.imageRect.extent.width = (int32_t)vr_xr_eye_width;
			views[eye].subImage.imageRect.extent.height = (int32_t)vr_xr_eye_height;
			views[eye].subImage.imageArrayIndex = 0;
		}

		memset (&layer, 0, sizeof (layer));
		layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		layer.space = xr_space;
		layer.viewCount = VR_XR_EYES;
		layer.views = views;

		layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
		end_info.layerCount = 1;
		end_info.layers = layers;
	}

	VR_XR_ReleaseEyes ();

	xrEndFrame (xr_session, &end_info);

	xr_eye_rendered[0] = xr_eye_rendered[1] = false;
}

/*
	INPUT

	The OpenXR action system is a layer of indirection over raw buttons: we
	declare abstract actions, suggest per-controller bindings, and the runtime
	resolves them. Users can then rebind in their runtime's own UI, and
	controllers we never tested still work if their profile is close enough.

	Action inventory mirrors quakevr's (vr.cpp:1204-1229), minus the menu set.

================================================================================
*/

vr_hand_t vr_xr_hand[VR_HANDS];

// Whether the previous frame produced a pose to blend from. Weapon weight has
// nothing to lag behind on the first tracked frame, or after tracking drops.
static qboolean xr_hand_pose_seen[VR_HANDS];

static XrActionSet xr_action_set = XR_NULL_HANDLE;
static XrPath	   xr_hand_path[VR_HANDS];

static XrAction xr_act_pose = XR_NULL_HANDLE;	 // grip: where the hand is
static XrAction xr_act_aim = XR_NULL_HANDLE;	 // aim: where it points
static XrAction xr_act_trigger = XR_NULL_HANDLE; // fire
static XrAction xr_act_grip = XR_NULL_HANDLE;	 // grab
static XrAction xr_act_stick = XR_NULL_HANDLE;	 // locomotion / turn
static XrAction xr_act_btn_a = XR_NULL_HANDLE;
static XrAction xr_act_btn_b = XR_NULL_HANDLE;
static XrAction xr_act_btn_stick = XR_NULL_HANDLE;
static XrAction xr_act_menu = XR_NULL_HANDLE;
static XrAction xr_act_haptic = XR_NULL_HANDLE;

static XrSpace xr_pose_space[VR_HANDS] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrSpace xr_aim_space[VR_HANDS] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

static qboolean xr_input_ready = false;

// Velocity history for throwing. quakevr averages the last N frames because the
// hand decelerates hard at the moment of release, so the instantaneous value at
// release badly under-reads what the player actually did.
// (quakevr vr.cpp:4708-4716, defaults from vr_cvars.cpp:178-179)
#define VR_MAX_THROW_FRAMES 32
static vec3_t xr_vel_history[VR_HANDS][VR_MAX_THROW_FRAMES];
static vec3_t xr_avel_history[VR_HANDS][VR_MAX_THROW_FRAMES];
static int	  xr_vel_head[VR_HANDS] = {0, 0};
static int	  xr_vel_count[VR_HANDS] = {0, 0};

/*
XR_PushVelocitySample
===============
*/
static void XR_PushVelocitySample (int hand, const vec3_t vel, const vec3_t avel)
{
	const int slot = xr_vel_head[hand];

	VectorCopy (vel, xr_vel_history[hand][slot]);
	VectorCopy (avel, xr_avel_history[hand][slot]);

	xr_vel_head[hand] = (slot + 1) % VR_MAX_THROW_FRAMES;
	if (xr_vel_count[hand] < VR_MAX_THROW_FRAMES)
		xr_vel_count[hand]++;
}

/*
XR_AverageVelocity

Mean of the most recent `frames` samples, newest first.
===============
*/
static void XR_AverageVelocity (int hand, int frames, vec3_t history[VR_HANDS][VR_MAX_THROW_FRAMES], vec3_t out)
{
	int i, n, idx;

	out[0] = out[1] = out[2] = 0.0f;

	n = q_min (frames, xr_vel_count[hand]);
	if (n <= 0)
		return;

	for (i = 1; i <= n; i++)
	{
		idx = (xr_vel_head[hand] - i + VR_MAX_THROW_FRAMES) % VR_MAX_THROW_FRAMES;
		out[0] += history[hand][idx][0];
		out[1] += history[hand][idx][1];
		out[2] += history[hand][idx][2];
	}

	out[0] /= (float)n;
	out[1] /= (float)n;
	out[2] /= (float)n;
}

/*
VR_XR_ResetThrowAvg

Clears the history, so a throw cannot inherit motion from before a teleport,
respawn or level change. (quakevr VR_ResetThrowAvgFrames)
===============
*/
void VR_XR_ResetThrowAvg (void)
{
	memset (xr_vel_history, 0, sizeof (xr_vel_history));
	memset (xr_avel_history, 0, sizeof (xr_avel_history));
	xr_vel_head[0] = xr_vel_head[1] = 0;
	xr_vel_count[0] = xr_vel_count[1] = 0;
}

/*
XR_Path
===============
*/
static XrPath XR_Path (const char *str)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath (xr_instance, str, &p);
	return p;
}

/*
XR_MakeAction
===============
*/
static XrAction XR_MakeAction (const char *name, const char *localized, XrActionType type)
{
	XrActionCreateInfo info;
	XrAction		   action = XR_NULL_HANDLE;

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_ACTION_CREATE_INFO;
	info.actionType = type;
	q_strlcpy (info.actionName, name, XR_MAX_ACTION_NAME_SIZE);
	q_strlcpy (info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
	// every action is per-hand; the runtime keeps left/right separate for us
	info.countSubactionPaths = VR_HANDS;
	info.subactionPaths = xr_hand_path;

	if (XR_FAILED (xrCreateAction (xr_action_set, &info, &action)))
	{
		Con_Warning ("OpenXR: could not create action %s\n", name);
		return XR_NULL_HANDLE;
	}
	return action;
}

/*
XR_SuggestProfile

Offers one controller profile's worth of bindings. Failure is not fatal: a
runtime may simply not know the profile, and another may still match.
===============
*/
static void XR_SuggestProfile (const char *profile, const XrActionSuggestedBinding *bindings, uint32_t count)
{
	XrInteractionProfileSuggestedBinding suggest;
	XrPath								 profile_path = XR_Path (profile);

	if (profile_path == XR_NULL_PATH)
		return;

	memset (&suggest, 0, sizeof (suggest));
	suggest.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggest.interactionProfile = profile_path;
	suggest.suggestedBindings = bindings;
	suggest.countSuggestedBindings = count;

	if (XR_SUCCEEDED (xrSuggestInteractionProfileBindings (xr_instance, &suggest)))
		Con_Printf ("OpenXR: bound %s\n", profile);
}

/*
VR_XR_InitInput
===============
*/
void VR_XR_InitInput (void)
{
	XrActionSetCreateInfo		  set_info;
	XrActionSpaceCreateInfo		  space_info;
	XrSessionActionSetsAttachInfo attach_info;
	uint32_t					  hand;

	if (!vr_xr_active || xr_session == XR_NULL_HANDLE)
		return;

	memset (vr_xr_hand, 0, sizeof (vr_xr_hand));

	xr_hand_path[VR_HAND_LEFT] = XR_Path ("/user/hand/left");
	xr_hand_path[VR_HAND_RIGHT] = XR_Path ("/user/hand/right");

	memset (&set_info, 0, sizeof (set_info));
	set_info.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	q_strlcpy (set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE);
	q_strlcpy (set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	set_info.priority = 0;

	if (XR_FAILED (xrCreateActionSet (xr_instance, &set_info, &xr_action_set)))
	{
		Con_Warning ("OpenXR: could not create action set; controllers disabled\n");
		return;
	}

	xr_act_pose = XR_MakeAction ("hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT);
	xr_act_aim = XR_MakeAction ("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
	xr_act_trigger = XR_MakeAction ("fire", "Fire", XR_ACTION_TYPE_FLOAT_INPUT);
	xr_act_grip = XR_MakeAction ("grab", "Grab", XR_ACTION_TYPE_FLOAT_INPUT);
	xr_act_stick = XR_MakeAction ("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
	xr_act_btn_a = XR_MakeAction ("button_a", "Button A", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr_act_btn_b = XR_MakeAction ("button_b", "Button B", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr_act_btn_stick = XR_MakeAction ("stick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr_act_menu = XR_MakeAction ("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr_act_haptic = XR_MakeAction ("haptic", "Haptic Feedback", XR_ACTION_TYPE_VIBRATION_OUTPUT);

	if (!xr_act_pose || !xr_act_aim)
	{
		Con_Warning ("OpenXR: pose actions unavailable; controllers disabled\n");
		return;
	}

	// --- Oculus Touch: the Quest controllers ---
	{
		const XrActionSuggestedBinding b[] = {
			{xr_act_pose, XR_Path ("/user/hand/left/input/grip/pose")},
			{xr_act_pose, XR_Path ("/user/hand/right/input/grip/pose")},
			{xr_act_aim, XR_Path ("/user/hand/left/input/aim/pose")},
			{xr_act_aim, XR_Path ("/user/hand/right/input/aim/pose")},
			{xr_act_trigger, XR_Path ("/user/hand/left/input/trigger/value")},
			{xr_act_trigger, XR_Path ("/user/hand/right/input/trigger/value")},
			{xr_act_grip, XR_Path ("/user/hand/left/input/squeeze/value")},
			{xr_act_grip, XR_Path ("/user/hand/right/input/squeeze/value")},
			{xr_act_stick, XR_Path ("/user/hand/left/input/thumbstick")},
			{xr_act_stick, XR_Path ("/user/hand/right/input/thumbstick")},
			{xr_act_btn_a, XR_Path ("/user/hand/left/input/x/click")},
			{xr_act_btn_a, XR_Path ("/user/hand/right/input/a/click")},
			{xr_act_btn_b, XR_Path ("/user/hand/left/input/y/click")},
			{xr_act_btn_b, XR_Path ("/user/hand/right/input/b/click")},
			{xr_act_btn_stick, XR_Path ("/user/hand/left/input/thumbstick/click")},
			{xr_act_btn_stick, XR_Path ("/user/hand/right/input/thumbstick/click")},
			{xr_act_menu, XR_Path ("/user/hand/left/input/menu/click")},
			{xr_act_haptic, XR_Path ("/user/hand/left/output/haptic")},
			{xr_act_haptic, XR_Path ("/user/hand/right/output/haptic")},
		};
		XR_SuggestProfile ("/interaction_profiles/oculus/touch_controller", b, countof (b));
	}

	// --- Valve Index ---
	{
		const XrActionSuggestedBinding b[] = {
			{xr_act_pose, XR_Path ("/user/hand/left/input/grip/pose")},
			{xr_act_pose, XR_Path ("/user/hand/right/input/grip/pose")},
			{xr_act_aim, XR_Path ("/user/hand/left/input/aim/pose")},
			{xr_act_aim, XR_Path ("/user/hand/right/input/aim/pose")},
			{xr_act_trigger, XR_Path ("/user/hand/left/input/trigger/value")},
			{xr_act_trigger, XR_Path ("/user/hand/right/input/trigger/value")},
			{xr_act_grip, XR_Path ("/user/hand/left/input/squeeze/value")},
			{xr_act_grip, XR_Path ("/user/hand/right/input/squeeze/value")},
			{xr_act_stick, XR_Path ("/user/hand/left/input/thumbstick")},
			{xr_act_stick, XR_Path ("/user/hand/right/input/thumbstick")},
			{xr_act_btn_a, XR_Path ("/user/hand/left/input/a/click")},
			{xr_act_btn_a, XR_Path ("/user/hand/right/input/a/click")},
			{xr_act_btn_b, XR_Path ("/user/hand/left/input/b/click")},
			{xr_act_btn_b, XR_Path ("/user/hand/right/input/b/click")},
			{xr_act_btn_stick, XR_Path ("/user/hand/left/input/thumbstick/click")},
			{xr_act_btn_stick, XR_Path ("/user/hand/right/input/thumbstick/click")},
			{xr_act_haptic, XR_Path ("/user/hand/left/output/haptic")},
			{xr_act_haptic, XR_Path ("/user/hand/right/output/haptic")},
		};
		XR_SuggestProfile ("/interaction_profiles/valve/index_controller", b, countof (b));
	}

	// --- generic fallback: every runtime must support this profile ---
	{
		const XrActionSuggestedBinding b[] = {
			{xr_act_pose, XR_Path ("/user/hand/left/input/grip/pose")},
			{xr_act_pose, XR_Path ("/user/hand/right/input/grip/pose")},
			{xr_act_aim, XR_Path ("/user/hand/left/input/aim/pose")},
			{xr_act_aim, XR_Path ("/user/hand/right/input/aim/pose")},
			{xr_act_btn_a, XR_Path ("/user/hand/left/input/select/click")},
			{xr_act_btn_a, XR_Path ("/user/hand/right/input/select/click")},
			{xr_act_menu, XR_Path ("/user/hand/left/input/menu/click")},
			{xr_act_menu, XR_Path ("/user/hand/right/input/menu/click")},
			{xr_act_haptic, XR_Path ("/user/hand/left/output/haptic")},
			{xr_act_haptic, XR_Path ("/user/hand/right/output/haptic")},
		};
		XR_SuggestProfile ("/interaction_profiles/khr/simple_controller", b, countof (b));
	}

	// pose actions each need a space before they can be located
	for (hand = 0; hand < VR_HANDS; hand++)
	{
		memset (&space_info, 0, sizeof (space_info));
		space_info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
		space_info.action = xr_act_pose;
		space_info.subactionPath = xr_hand_path[hand];
		space_info.poseInActionSpace.orientation.w = 1.0f;
		xrCreateActionSpace (xr_session, &space_info, &xr_pose_space[hand]);

		space_info.action = xr_act_aim;
		xrCreateActionSpace (xr_session, &space_info, &xr_aim_space[hand]);
	}

	memset (&attach_info, 0, sizeof (attach_info));
	attach_info.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attach_info.countActionSets = 1;
	attach_info.actionSets = &xr_action_set;
	if (XR_FAILED (xrAttachSessionActionSets (xr_session, &attach_info)))
	{
		Con_Warning ("OpenXR: could not attach action set; controllers disabled\n");
		return;
	}

	XR_InitHandTracking (); // optional; falls back to grip-driven fingers

	xr_input_ready = true;
	Con_Printf ("OpenXR: controllers ready\n");
}

/*
XR_ReadFloat
===============
*/
static float XR_ReadFloat (XrAction action, uint32_t hand)
{
	XrActionStateGetInfo info;
	XrActionStateFloat	 state;

	if (!action)
		return 0.0f;

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_ACTION_STATE_GET_INFO;
	info.action = action;
	info.subactionPath = xr_hand_path[hand];

	memset (&state, 0, sizeof (state));
	state.type = XR_TYPE_ACTION_STATE_FLOAT;

	if (XR_FAILED (xrGetActionStateFloat (xr_session, &info, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

/*
XR_ReadBool
===============
*/
static qboolean XR_ReadBool (XrAction action, uint32_t hand)
{
	XrActionStateGetInfo info;
	XrActionStateBoolean state;

	if (!action)
		return false;

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_ACTION_STATE_GET_INFO;
	info.action = action;
	info.subactionPath = xr_hand_path[hand];

	memset (&state, 0, sizeof (state));
	state.type = XR_TYPE_ACTION_STATE_BOOLEAN;

	if (XR_FAILED (xrGetActionStateBoolean (xr_session, &info, &state)) || !state.isActive)
		return false;
	return state.currentState ? true : false;
}

/*
XR_ReadStick
===============
*/
static void XR_ReadStick (XrAction action, uint32_t hand, float out[2])
{
	XrActionStateGetInfo  info;
	XrActionStateVector2f state;

	out[0] = out[1] = 0.0f;
	if (!action)
		return;

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_ACTION_STATE_GET_INFO;
	info.action = action;
	info.subactionPath = xr_hand_path[hand];

	memset (&state, 0, sizeof (state));
	state.type = XR_TYPE_ACTION_STATE_VECTOR2F;

	if (XR_FAILED (xrGetActionStateVector2f (xr_session, &info, &state)) || !state.isActive)
		return;
	out[0] = state.currentState.x;
	out[1] = state.currentState.y;
}

/*
XR_LocateHand

Same coordinate conversion the head uses (quakevr vr.cpp:3511).
===============
*/
static qboolean XR_LocateHand (XrSpace space, XrTime time, vec3_t out_pos, vec3_t out_angles, vec3_t out_vel)
{
	XrSpaceLocation loc;
	XrSpaceVelocity vel;
	float			scale = VR_METERS_TO_UNITS;

	if (space == XR_NULL_HANDLE)
		return false;

	memset (&vel, 0, sizeof (vel));
	vel.type = XR_TYPE_SPACE_VELOCITY;

	memset (&loc, 0, sizeof (loc));
	loc.type = XR_TYPE_SPACE_LOCATION;
	// ask for velocity alongside the pose: the runtime's own figure is far
	// steadier than differencing positions across frames
	if (out_vel)
		loc.next = &vel;

	if (XR_FAILED (xrLocateSpace (space, xr_space, time, &loc)))
		return false;

	if (out_vel)
	{
		out_vel[0] = out_vel[1] = out_vel[2] = 0.0f;
		if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
		{
			out_vel[0] = -vel.linearVelocity.z * scale;
			out_vel[1] = -vel.linearVelocity.x * scale;
			out_vel[2] = vel.linearVelocity.y * scale;
		}
	}

	if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return false;

	out_pos[0] = -loc.pose.position.z * scale;
	out_pos[1] = -loc.pose.position.x * scale;
	out_pos[2] = loc.pose.position.y * scale + vr_floor_offset.value + vr_gun_z_offset.value;

	XR_QuatToQuakeAngles (&loc.pose.orientation, out_angles);
	return true;
}

/*
	FINGER TRACKING

	quakevr reads OpenVR's skeletal summary, which hands back a 0..1 curl per
	finger, and turns it into a frame number 0..5 on the finger models it ships.
	OpenXR's equivalent is XR_EXT_hand_tracking, which reports joint poses
	rather than a curl, so the curl is derived from how far each fingertip has
	folded toward the palm.

	Everything downstream -- the frame mapping, thumb auto-close, grip bias and
	blending -- follows quakevr (vr.cpp:3270-3354).

================================================================================
*/

// finger indices, matching quakevr's FingerIdx order
#define VR_FINGER_THUMB	 0
#define VR_FINGER_INDEX	 1
#define VR_FINGER_MIDDLE 2
#define VR_FINGER_RING	 3
#define VR_FINGER_PINKY	 4

static XrHandTrackerEXT xr_hand_tracker[VR_HANDS] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static qboolean			xr_hand_tracking_ready = false;

static PFN_xrCreateHandTrackerEXT  pfn_xrCreateHandTrackerEXT = NULL;
static PFN_xrDestroyHandTrackerEXT pfn_xrDestroyHandTrackerEXT = NULL;
static PFN_xrLocateHandJointsEXT   pfn_xrLocateHandJointsEXT = NULL;

/*
XR_InitHandTracking

Optional: plenty of runtimes and controllers do not report joints. When absent
the analog grip drives the fingers instead, which is what a Touch controller
can actually express anyway.
===============
*/
static void XR_InitHandTracking (void)
{
	XrHandTrackerCreateInfoEXT info;
	uint32_t				   hand;

	pfn_xrCreateHandTrackerEXT = (PFN_xrCreateHandTrackerEXT)XR_GetProc ("xrCreateHandTrackerEXT");
	pfn_xrDestroyHandTrackerEXT = (PFN_xrDestroyHandTrackerEXT)XR_GetProc ("xrDestroyHandTrackerEXT");
	pfn_xrLocateHandJointsEXT = (PFN_xrLocateHandJointsEXT)XR_GetProc ("xrLocateHandJointsEXT");

	if (!pfn_xrCreateHandTrackerEXT || !pfn_xrLocateHandJointsEXT)
		return; // runtime does not offer it

	for (hand = 0; hand < VR_HANDS; hand++)
	{
		memset (&info, 0, sizeof (info));
		info.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;
		info.hand = (hand == VR_HAND_LEFT) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
		info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

		if (XR_FAILED (pfn_xrCreateHandTrackerEXT (xr_session, &info, &xr_hand_tracker[hand])))
		{
			xr_hand_tracker[hand] = XR_NULL_HANDLE;
			return;
		}
	}

	xr_hand_tracking_ready = true;
	Con_Printf ("OpenXR: finger tracking available\n");
}

/*
XR_CurlToFrame

quakevr's handSkeletalToFrame (vr.cpp:3270-3299): bias, clamp, scale to the
model's five frames, with the thumb closing automatically once the other
fingers are mostly curled -- otherwise a fist reads as a thumbs-up.
===============
*/
static float XR_CurlToFrame (const float curl[5], int finger)
{
	if (finger == VR_FINGER_THUMB && vr_finger_auto_close_thumb.value)
	{
		const float avg = (curl[VR_FINGER_INDEX] + curl[VR_FINGER_MIDDLE] + curl[VR_FINGER_RING] + curl[VR_FINGER_PINKY]) * 0.25f;
		if (avg > 0.5f)
			return 5.0f;
	}

	return CLAMP (0.0f, curl[finger] + vr_finger_grip_bias.value, 1.0f) * 5.0f;
}

/*
XR_UpdateFingers

Blending is quakevr's: move toward the target at a fixed rate rather than
snapping, so fingers do not pop between frames. (vr.cpp:3308-3341)
===============
*/
/*
===============
XR_WeaponWeightFactor

quakevr's VR_GetWeaponWeightFactorImpl (vr.cpp). aiming2H is 0 when the weapon
is held in one hand and 1 when it is fully two-handed, and the two branches are
lerped between so that bringing the second hand up steadies the weapon.

Per-weapon Weight, Weight2HPosMult and Weight2HDirMult are 0.0, 1.0 and 1.0 for
every weapon in quakevr -- InitWeaponCVars defaults them and no call overrides
-- so they are those constants here. If per-weapon weights are ever wanted, this
is where they multiply in.
===============
*/
static float XR_WeaponWeightFactor (float aiming2h, float weight_offset, float weight_mult, float twoh_help_offset, float twoh_help_mult)
{
	const float wpn_weight = 0.0f;	  // WpnCVar::Weight
	const float wpn_twoh_mult = 1.0f; // WpnCVar::Weight2H{Pos,Dir}Mult
	float		initial, with_offset, with_mult, with_2h_offset, with_2h_mult, final_factor;

	initial = 1.0f - wpn_weight;
	with_offset = initial + weight_offset;
	with_mult = with_offset * weight_mult;
	with_2h_offset = with_offset + twoh_help_offset;
	with_2h_mult = (with_2h_offset * twoh_help_mult) * wpn_twoh_mult;

	final_factor = with_mult + (with_2h_mult - with_mult) * aiming2h;
	return CLAMP (0.0f, final_factor, 1.0f);
}

/*
XR_WeaponWeightBlend

The factor, adjusted for frametime the way quakevr does it
(VR_CalcWeaponWeightFTAdjusted): weight * frametime * 100. At 90fps and stock
weights that lands just above 1, which is a complete blend -- no smoothing --
so it is clamped rather than allowed to overshoot the target.
===============
*/
static float XR_WeaponWeightBlend (qboolean is_dir)
{
	float factor, blend;

	if (is_dir)
		factor = XR_WeaponWeightFactor (
			0.0f, vr_wpn_dir_weight_offset.value, vr_wpn_dir_weight_mult.value, vr_wpn_dir_weight_2h_help_offset.value,
			vr_wpn_dir_weight_2h_help_mult.value);
	else
		factor = XR_WeaponWeightFactor (
			0.0f, vr_wpn_pos_weight_offset.value, vr_wpn_pos_weight_mult.value, vr_wpn_pos_weight_2h_help_offset.value,
			vr_wpn_pos_weight_2h_help_mult.value);

	blend = factor * (float)(cl.time - cl.oldtime) * 100.0f;
	return CLAMP (0.0f, blend, 1.0f);
}

/*
XR_ApplyWeaponWeight

Lags the hand behind the controller according to the weapon's weight.

quakevr smooths cl.handpos, which is in world space, and therefore has to undo
the player's own movement and yaw first -- that is what the rotate_point and
lastPlayerTranslation work in vr.cpp:1908-1950 is for. Smoothing in tracking
space instead makes all of that unnecessary: tracking space does not move with
the player, so walking and turning cannot leak into the blend to begin with.
===============
*/
static void XR_ApplyWeaponWeight (vr_hand_t *h, qboolean had_prev)
{
	float t;
	int	  i;

	if (!had_prev || cl.time <= cl.oldtime)
	{
		VectorCopy (h->pos, h->weighted_pos);
		VectorCopy (h->aim_pos, h->weighted_aim_pos);
		VectorCopy (h->aim_angles, h->weighted_aim_angles);
		return;
	}

	if (vr_wpn_pos_weight.value == 1)
	{
		t = XR_WeaponWeightBlend (false);
		for (i = 0; i < 3; i++)
		{
			h->weighted_pos[i] += (h->pos[i] - h->weighted_pos[i]) * t;
			h->weighted_aim_pos[i] += (h->aim_pos[i] - h->weighted_aim_pos[i]) * t;
		}
	}
	else
	{
		VectorCopy (h->pos, h->weighted_pos);
		VectorCopy (h->aim_pos, h->weighted_aim_pos);
	}

	if (vr_wpn_dir_weight.value == 1)
	{
		t = XR_WeaponWeightBlend (true);
		for (i = 0; i < 3; i++)
		{
			// shortest way round, so 359 -> 1 does not sweep the long way
			float d = h->aim_angles[i] - h->weighted_aim_angles[i];
			while (d > 180.0f)
				d -= 360.0f;
			while (d < -180.0f)
				d += 360.0f;
			h->weighted_aim_angles[i] += d * t;
		}
	}
	else
	{
		VectorCopy (h->aim_angles, h->weighted_aim_angles);
	}

	// the weighted pose is what everything downstream should see
	VectorCopy (h->weighted_pos, h->pos);
	VectorCopy (h->weighted_aim_pos, h->aim_pos);
	VectorCopy (h->weighted_aim_angles, h->aim_angles);
}

static void XR_UpdateFingers (int hand)
{
	vr_hand_t *h = &vr_xr_hand[hand];
	float	   curl[5];
	int		   i;

	// Default: drive every finger from the analog grip, which is all a Touch
	// controller reports. Trigger drives the index separately so pointing works.
	for (i = 0; i < 5; i++)
		curl[i] = h->grip;
	curl[VR_FINGER_INDEX] = q_max (h->grip, h->trigger);

	if (xr_hand_tracking_ready && xr_hand_tracker[hand] != XR_NULL_HANDLE)
	{
		XrHandJointsLocateInfoEXT	locate;
		XrHandJointLocationsEXT		locations;
		XrHandJointLocationEXT		joints[XR_HAND_JOINT_COUNT_EXT];

		memset (&joints, 0, sizeof (joints));
		memset (&locations, 0, sizeof (locations));
		locations.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
		locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
		locations.jointLocations = joints;

		memset (&locate, 0, sizeof (locate));
		locate.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT;
		locate.baseSpace = xr_space;
		locate.time = xr_frame_state.predictedDisplayTime;

		if (XR_SUCCEEDED (pfn_xrLocateHandJointsEXT (xr_hand_tracker[hand], &locate, &locations)) && locations.isActive)
		{
			// tip and metacarpal joint per finger, in XR_HAND_JOINT_* order
			static const int tip[5] = {
				XR_HAND_JOINT_THUMB_TIP_EXT, XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_RING_TIP_EXT,
				XR_HAND_JOINT_LITTLE_TIP_EXT};
			static const int base[5] = {
				XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
				XR_HAND_JOINT_RING_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT};
			const XrVector3f *palm = &joints[XR_HAND_JOINT_PALM_EXT].pose.position;

			for (i = 0; i < 5; i++)
			{
				XrVector3f	 dt, db;
				float		 lt, lb;

				dt.x = joints[tip[i]].pose.position.x - palm->x;
				dt.y = joints[tip[i]].pose.position.y - palm->y;
				dt.z = joints[tip[i]].pose.position.z - palm->z;
				db.x = joints[base[i]].pose.position.x - palm->x;
				db.y = joints[base[i]].pose.position.y - palm->y;
				db.z = joints[base[i]].pose.position.z - palm->z;

				lt = sqrtf (dt.x * dt.x + dt.y * dt.y + dt.z * dt.z);
				lb = sqrtf (db.x * db.x + db.y * db.y + db.z * db.z);

				// an extended finger puts its tip much further from the palm
				// than its knuckle; a curled one brings the two together
				if (lb > 0.0001f)
					curl[i] = CLAMP (0.0f, 1.0f - ((lt / lb) - 1.0f), 1.0f);
			}
		}
	}

	for (i = 0; i < 5; i++)
	{
		const float target = XR_CurlToFrame (curl, i);

		if (!vr_finger_blending.value)
		{
			h->finger[i] = target;
			continue;
		}

		{
			const float speed = (float)host_frametime * vr_finger_blending_speed.value;
			if (h->finger[i] > target)
			{
				h->finger[i] -= speed;
				if (h->finger[i] < target)
					h->finger[i] = target;
			}
			else if (h->finger[i] < target)
			{
				h->finger[i] += speed;
				if (h->finger[i] > target)
					h->finger[i] = target;
			}
		}
	}
}

/*
VR_XR_SyncInput
===============
*/
void VR_XR_SyncInput (void)
{
	XrActionsSyncInfo sync;
	XrActiveActionSet active;
	uint32_t		  hand;

	if (!xr_input_ready || !VR_XR_SessionRunning ())
		return;

	memset (&active, 0, sizeof (active));
	active.actionSet = xr_action_set;
	active.subactionPath = XR_NULL_PATH;

	memset (&sync, 0, sizeof (sync));
	sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;

	if (XR_FAILED (xrSyncActions (xr_session, &sync)))
		return;
	// the head's own velocity, which room-scale jumping reads
	if (xr_view_space != XR_NULL_HANDLE)
	{
		vec3_t hp, ha;
		xr_head_vel_valid = XR_LocateHand (xr_view_space, xr_frame_state.predictedDisplayTime, hp, ha, xr_head_velocity);
	}
	else
		xr_head_vel_valid = false;


	for (hand = 0; hand < VR_HANDS; hand++)
	{
		vr_hand_t *h = &vr_xr_hand[hand];

		// located at predicted display time, like the head, so hands and view
		// agree rather than drifting apart by a frame
		h->tracked = XR_LocateHand (xr_pose_space[hand], xr_frame_state.predictedDisplayTime, h->pos, h->angles, h->velocity);
		XR_LocateHand (xr_aim_space[hand], xr_frame_state.predictedDisplayTime, h->aim_pos, h->aim_angles, NULL);
		// weight the pose before anything reads it, so the weapon, the hand
		// models and the QC all see the same lagged pose rather than three
		// different ones
		XR_ApplyWeaponWeight (h, xr_hand_pose_seen[hand]);
		xr_hand_pose_seen[hand] = h->tracked;
		h->speed = sqrtf (h->velocity[0] * h->velocity[0] + h->velocity[1] * h->velocity[1] + h->velocity[2] * h->velocity[2]);

		// throwing reads a rolling mean, not this frame's value
		XR_PushVelocitySample (hand, h->velocity, h->angular_velocity);
		XR_AverageVelocity (hand, (int)vr_throw_avg_frames.value, xr_vel_history, h->throw_velocity);

		h->trigger = XR_ReadFloat (xr_act_trigger, hand);
		h->grip = XR_ReadFloat (xr_act_grip, hand);
		XR_ReadStick (xr_act_stick, hand, h->stick);
		h->btn_a = XR_ReadBool (xr_act_btn_a, hand);
		h->btn_b = XR_ReadBool (xr_act_btn_b, hand);
		h->btn_stick = XR_ReadBool (xr_act_btn_stick, hand);
		h->btn_menu = XR_ReadBool (xr_act_menu, hand);

		XR_UpdateFingers ((int)hand);
	}

	// The per-weapon offsets are quakevr's, and they are expressed relative to
	// the OpenVR controller pose plus vr_gunangle (32 degrees). OpenXR's grip
	// pose is the same device-native frame, so grip + 32 should reproduce it --
	// but runtimes vary in how they define grip, and the difference between
	// grip and aim IS the runtime stating its convention. Report it once so the
	// correction can be a measured constant rather than a guess.
	{
		static qboolean reported = false;
		const int		mh = VR_XR_MainHand ();
		if (!reported && vr_xr_hand[mh].tracked)
		{
			reported = true;
			Con_Printf (
				"OpenXR: grip->aim delta  pitch %.1f  yaw %.1f  roll %.1f\n", vr_xr_hand[mh].aim_angles[PITCH] - vr_xr_hand[mh].angles[PITCH],
				vr_xr_hand[mh].aim_angles[YAW] - vr_xr_hand[mh].angles[YAW], vr_xr_hand[mh].aim_angles[ROLL] - vr_xr_hand[mh].angles[ROLL]);
		}
	}
}

/*
	GAMEPLAY

	Locomotion is head-relative: push the stick forward and you go where you are
	looking, which is what almost every VR shooter does and what feels right
	when your head and body can point different ways.

================================================================================
*/

cvar_t vr_turn_speed = {"vr_turn_speed", "120", CVAR_ARCHIVE}; // degrees/sec, smooth turning
cvar_t vr_snap_turn = {"vr_snap_turn", "45", CVAR_ARCHIVE};	   // degrees per snap; 0 = smooth
cvar_t vr_deadzone = {"vr_deadzone", "0.2", CVAR_ARCHIVE};

// Room-scale: physically walking moves the player, not just the camera.
cvar_t vr_roomscale = {"vr_roomscale", "1", CVAR_ARCHIVE};
cvar_t vr_roomscale_mult = {"vr_roomscale_mult", "1", CVAR_ARCHIVE};

// Point the weapon with the controller instead of the head.
cvar_t vr_hand_aiming = {"vr_hand_aiming", "1", CVAR_ARCHIVE};

// Swaps the controller roles, so the weapon is held in the left hand and the
// off-hand functions move to the right. quakevr defaults this off and applies
// it where the controller is identified (vr_cvars.cpp:36, vr.cpp:2737-2748).
cvar_t vr_lefthanded = {"vr_lefthanded", "0", CVAR_ARCHIVE};

// Derived from vr_lefthanded; kept as a cvar so it can still be forced.
cvar_t vr_aim_hand = {"vr_aim_hand", "1", CVAR_ARCHIVE}; // 1 = right

/*
VR_XR_MainHand / VR_XR_OffHand

One place that decides which physical controller is which. Everything that
cares -- aiming, the weapon model, melee, weapon switching, teleport, holster
hotspots -- goes through these, so handedness cannot end up inconsistent
between systems.
===============
*/
int VR_XR_MainHand (void)
{
	if (vr_lefthanded.value)
		return VR_HAND_LEFT;
	return vr_aim_hand.value ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

int VR_XR_OffHand (void)
{
	return VR_XR_MainHand () ^ 1;
}

// Where the gun model sits relative to the hand. Quake's viewmodels have their
// origin at the shoulder end rather than the grip, so it needs pushing forward.
cvar_t vr_gun_offset_x = {"vr_gun_offset_x", "0", CVAR_ARCHIVE};
cvar_t vr_gun_offset_y = {"vr_gun_offset_y", "0", CVAR_ARCHIVE};
cvar_t vr_gun_offset_z = {"vr_gun_offset_z", "0", CVAR_ARCHIVE};

// Quake units/sec the hand must be moving to register a melee swing.
cvar_t vr_melee_threshold = {"vr_melee_threshold", "120", CVAR_ARCHIVE};
cvar_t vr_haptics = {"vr_haptics", "1", CVAR_ARCHIVE};

// Laser dot: hand aiming is close to unusable without some feedback about
// where the gun is actually pointing.
cvar_t vr_laser = {"vr_laser", "1", CVAR_ARCHIVE};

// Two-handed aiming: grip with the off hand to steady a long gun. Aim then runs
// along the line between the hands rather than one controller's ray, which is
// both steadier and how you would actually hold a rifle.
cvar_t vr_two_handed = {"vr_two_handed", "1", CVAR_ARCHIVE};
cvar_t vr_two_hand_dist = {"vr_two_hand_dist", "24", CVAR_ARCHIVE}; // max hand separation, Quake units

// Player standing height in metres, used by "vr_calibrate".
cvar_t vr_height_calibration = {"vr_height_calibration", "1.646099", CVAR_ARCHIVE};

// Global gun model tweaks, on top of the per-weapon table.
cvar_t vr_gunmodelscale = {"vr_gunmodelscale", "0.7", CVAR_ARCHIVE};
cvar_t vr_gunmodely = {"vr_gunmodely", "1.3", CVAR_ARCHIVE};

// VR body and leg holsters. Defaults are quakevr's (vr_cvars.cpp:123-145).
cvar_t vr_vrtorso_enabled = {"vr_vrtorso_enabled", "1", CVAR_ARCHIVE};
cvar_t vr_vrtorso_x_offset = {"vr_vrtorso_x_offset", "-2.75", CVAR_ARCHIVE};
cvar_t vr_vrtorso_y_offset = {"vr_vrtorso_y_offset", "-4.75", CVAR_ARCHIVE};
// Not quakevr's -21: that belongs to its head_z_mult formula, which the change
// of world scale breaks. The torso now hangs from the head by the model's own
// height, so this is a nudge from there and starts at zero.
// quakevr ships -45, which puts the top of the torso exactly at the eyes. That
// is right for the author, whose vr_height_calibration is 1.646099; a taller
// player needs it lower, because the head_z_mult term scales with head height.
// Measured here: top landed at head +0.6, so -51 drops the neck about 5 units
// below the eyes, which is where a neck goes.
cvar_t vr_vrtorso_z_offset = {"vr_vrtorso_z_offset", "-51", CVAR_ARCHIVE};
cvar_t vr_vrtorso_head_z_mult = {"vr_vrtorso_head_z_mult", "33", CVAR_ARCHIVE};
cvar_t vr_vrtorso_x_scale = {"vr_vrtorso_x_scale", "0.675", CVAR_ARCHIVE};
cvar_t vr_vrtorso_y_scale = {"vr_vrtorso_y_scale", "0.675", CVAR_ARCHIVE};
cvar_t vr_vrtorso_z_scale = {"vr_vrtorso_z_scale", "1.1", CVAR_ARCHIVE};
cvar_t vr_vrtorso_pitch = {"vr_vrtorso_pitch", "1.5", CVAR_ARCHIVE};
cvar_t vr_vrtorso_yaw = {"vr_vrtorso_yaw", "0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_roll = {"vr_vrtorso_roll", "0", CVAR_ARCHIVE};

cvar_t vr_leg_holster_model_enabled = {"vr_leg_holster_model_enabled", "1", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_scale = {"vr_leg_holster_model_scale", "0.5", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_x_offset = {"vr_leg_holster_model_x_offset", "1", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_y_offset = {"vr_leg_holster_model_y_offset", "1.25", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_z_offset = {"vr_leg_holster_model_z_offset", "2.25", CVAR_ARCHIVE};

/*
	WEAPON MODEL OFFSETS

	Quake's viewmodels are built to sit at the bottom-right of a flat screen,
	not to be held. Each one needs its own offset and scale to end up in the
	player's hand pointing the right way.

	Every number below is quakevr's (vr.cpp:1029-1064). They were tuned against
	real play over a long time and there is nothing to be gained by re-deriving
	them.

================================================================================
*/

typedef struct
{
	const char *model;
	float		ofs_x, ofs_y, ofs_z;
	float		scale;
} vr_wpn_offset_t;

/*
VR_XR_GetScaleCorrect

quakevr's model numbers were all authored when the default world scale was
0.75, so every one of them is rescaled to whatever is in use now. Applies to
the body models as much as the weapons. (quakevr VR_GetScaleCorrect)
===============
*/
static float VR_XR_GetScaleCorrect (void)
{
	return (vr_world_scale.value / 0.75f) * vr_gunmodelscale.value;
}

// vanilla Quake, Scourge of Armagon, Dissolution of Eternity
static const vr_wpn_offset_t vr_wpn_offsets_id1[] = {
	{"progs/v_axe.mdl", -4.0f, 24.0f, 37.0f, 0.33f},
	{"progs/v_shot.mdl", 1.5f, 1.0f, 10.0f, 0.5f},	  // gun
	{"progs/v_shot2.mdl", -3.5f, 1.0f, 8.5f, 0.8f},	  // shotgun
	{"progs/v_nail.mdl", -5.0f, 3.0f, 15.0f, 0.5f},	  // nailgun
	{"progs/v_nail2.mdl", 0.0f, 3.0f, 19.0f, 0.5f},	  // supernailgun
	{"progs/v_rock.mdl", 10.0f, 1.5f, 13.0f, 0.5f},	  // grenade
	{"progs/v_rock2.mdl", 10.0f, 7.0f, 19.0f, 0.5f},  // rocket
	{"progs/v_light.mdl", 3.0f, 4.0f, 13.0f, 0.5f},	  // lightning
	{"progs/v_hammer.mdl", -4.0f, 18.0f, 37.0f, 0.33f},	 // mjolnir
	{"progs/v_laserg.mdl", 65.0f, 3.7f, 17.0f, 0.33f},	 // laser
	{"progs/v_prox.mdl", 10.0f, 1.5f, 13.0f, 0.5f},		 // proximity
	{"progs/v_lava.mdl", -5.0f, 3.0f, 15.0f, 0.5f},		 // lava nailgun
	{"progs/v_lava2.mdl", 0.0f, 3.0f, 19.0f, 0.5f},		 // lava supernailgun
	{"progs/v_multi.mdl", 10.0f, 1.5f, 13.0f, 0.5f},	 // multigrenade
	{"progs/v_multi2.mdl", 10.0f, 7.0f, 19.0f, 0.5f},	 // multirocket
	{"progs/v_plasma.mdl", 3.0f, 4.0f, 13.0f, 0.5f},	 // plasma
	{"progs/hand.mdl", 0.0f, 0.0f, 0.0f, 0.0f},
	{"progs/v_grpple.mdl", 0.0f, 0.0f, 0.0f, 0.0f},
};

// Arcane Dimensions, tuned against v1.70 + patch1
static const vr_wpn_offset_t vr_wpn_offsets_ad[] = {
	{"progs/v_shadaxe0.mdl", -1.5f, 43.1f, 41.0f, 0.25f}, // shadow axe
	{"progs/v_shadaxe3.mdl", -1.5f, 43.1f, 41.0f, 0.25f}, // shadow axe upgrade
	{"progs/v_shot.mdl", 1.5f, 1.7f, 17.5f, 0.33f},		  // shotgun
	{"progs/v_shot2.mdl", -3.5f, 0.4f, 8.5f, 0.8f},		  // double barrel
	{"progs/v_shot3.mdl", -3.5f, 0.4f, 8.5f, 0.8f},		  // Widowmaker
	{"progs/v_nail.mdl", -9.5f, 3.0f, 17.0f, 0.5f},
	{"progs/v_nail2.mdl", -6.0f, 3.5f, 20.0f, 0.4f},
	{"progs/v_rock.mdl", -3.0f, 1.25f, 17.0f, 0.5f},
	{"progs/v_rock2.mdl", 0.0f, 5.55f, 22.5f, 0.45f},
	{"progs/v_light.mdl", -4.0f, 3.1f, 13.0f, 0.5f},
	{"progs/v_plasma.mdl", 2.8f, 1.8f, 22.5f, 0.5f},
};

/*
VR_FindWpnOffset

Arcane Dimensions reuses stock model names with different geometry, so the
table is chosen by game directory exactly as quakevr does (vr.cpp:1027).
===============
*/
static const vr_wpn_offset_t *VR_FindWpnOffset (const char *model_name)
{
	const vr_wpn_offset_t *table;
	size_t				   count, i;

	if (!model_name || !*model_name)
		return NULL;

	if (!strcmp (COM_SkipPath (com_gamedir), "ad"))
	{
		table = vr_wpn_offsets_ad;
		count = countof (vr_wpn_offsets_ad);
	}
	else
	{
		table = vr_wpn_offsets_id1;
		count = countof (vr_wpn_offsets_id1);
	}

	for (i = 0; i < count; i++)
		if (!q_strcasecmp (table[i].model, model_name))
			return &table[i];

	return NULL;
}

/*
VR_XR_ApplyWeaponModelMod

Rewrites the model header's scale and offset, which is where Quake already
applies a per-model transform, so nothing in the render path has to change.
Always derived from the original values rather than the current ones, or the
edit would compound every frame. (quakevr VR_ApplyModelMod, vr.cpp:614-622)
===============
*/
void VR_XR_ApplyWeaponModelMod (aliashdr_t *hdr, const char *model_name)
{
	const vr_wpn_offset_t *w;
	float				   correct;
	int					   i;

	if (!hdr)
		return;

	correct = VR_XR_GetScaleCorrect ();

	w = VR_FindWpnOffset (model_name);

	// NB: deliberately not gated on the session running.
	//
	// This used to also reset when !VR_XR_SessionRunning(), but the session
	// reports not-running whenever the headset is idle or off the head, and
	// this only ran at map load. Loading a map in that state stripped every
	// weapon offset for the rest of the map, with nothing to re-apply it --
	// losing 10 to 41 units of offset and a 0.25-0.8 scale factor. quakevr
	// applies the mod unconditionally (vr.cpp:670-676) for exactly this reason.
	if (!w)
	{
		// genuinely not a weapon we have numbers for: leave the model alone
		for (i = 0; i < 3; i++)
		{
			hdr->scale[i] = hdr->original_scale[i];
			hdr->scale_origin[i] = hdr->original_scale_origin[i];
		}
		return;
	}

	// A model whose originals were never snapshotted (MD5/MD3 loaders do not
	// set them) would otherwise be scaled to zero and vanish.
	if (hdr->original_scale[0] == 0.0f && hdr->original_scale[1] == 0.0f && hdr->original_scale[2] == 0.0f)
		return;

	// Scale is always applied. It is a plain multiplier with no direction to
	// it, so it carries across from quakevr unchanged -- and it matters: the
	// table runs from 0.25 to 0.8, so dropping it draws every weapon between
	// 1.25x and 4x too large.
	for (i = 0; i < 3; i++)
		hdr->scale[i] = hdr->original_scale[i] * w->scale * correct;

	// The position offset is the part that does not carry across, because it
	// is measured in quakevr's controller frame. See the vr_wpn_offsets note.
	if (vr_wpn_offsets.value)
	{
		hdr->scale_origin[0] = (hdr->original_scale_origin[0] + w->ofs_x) * correct;
		hdr->scale_origin[1] = (hdr->original_scale_origin[1] + w->ofs_y) * correct;
		hdr->scale_origin[2] = (hdr->original_scale_origin[2] + w->ofs_z + vr_gunmodely.value) * correct;
	}
	else
	{
		for (i = 0; i < 3; i++)
			hdr->scale_origin[i] = hdr->original_scale_origin[i] * correct;
	}
}

/*
VR_XR_ModAllWeapons

Walks the offset table and applies each entry to its model, the same shape as
quakevr's VR_ModAllWeapons (vr.cpp:1150-1177). Called after the client has
precached a map's models.
===============
*/
/*
===============
XR_ModModel

Applies a uniform-ish scale and offset to a named model, the same way weapons
are handled. Missing models are not an error: a game may simply not ship them.
===============
*/
static void XR_ModModel (const char *name, const vec3_t scale, const vec3_t offset)
{
	qmodel_t   *model;
	aliashdr_t *hdr;
	int			i;

	model = Mod_ForName (name, false);
	if (!model || model->type != mod_alias)
		return;

	hdr = (aliashdr_t *)Mod_Extradata (model);
	if (!hdr)
		return;

	// Same scale correction the weapons get. quakevr runs the torso and the leg
	// holsters through the very same VR_ApplyModelMod (vr.cpp:614-622), so they
	// pick up VR_GetScaleCorrect too -- it is not a weapon-only factor. Leaving
	// it out drew both at 1/1.333, three quarters of their proper size, with
	// the offsets in unscaled units on top of that.
	const float correct = VR_XR_GetScaleCorrect ();

	for (i = 0; i < 3; i++)
	{
		hdr->scale[i] = hdr->original_scale[i] * scale[i] * correct;
		hdr->scale_origin[i] = (hdr->original_scale_origin[i] + offset[i]) * correct;
	}
}

/*
VR_XR_ModAllModels

quakevr's VR_ModAllModels: the weapons, plus the player's own body and the leg
holsters, which are ordinary models positioned by the QC.
(quakevr vr.cpp:1128-1152, 1180-1185)
===============
*/
void VR_XR_ModAllModels (void)
{
	vec3_t scale, offset;

	// Deliberately not gated on vr_xr_active, for the same reason
	// VR_XR_ApplyWeaponModelMod is not gated on the session running: this runs
	// at map load, and the headset reports inactive whenever it is idle or off
	// the head. Loading a map in that state would leave every model unmodified
	// for the rest of the map with nothing to re-apply it. quakevr calls
	// VR_ModAllModels unconditionally (common.cpp:2421, host.cpp:1299).
	if (vr_vrtorso_enabled.value)
	{
		scale[0] = vr_vrtorso_x_scale.value;
		scale[1] = vr_vrtorso_y_scale.value;
		scale[2] = vr_vrtorso_z_scale.value;
		offset[0] = offset[1] = offset[2] = 0.0f; // quakevr passes vec3_zero here
		XR_ModModel ("progs/vrtorso.mdl", scale, offset);
	}

	if (vr_leg_holster_model_enabled.value)
	{
		const float f = vr_leg_holster_model_scale.value;
		scale[0] = scale[1] = scale[2] = f;
		offset[0] = vr_leg_holster_model_x_offset.value;
		offset[1] = vr_leg_holster_model_y_offset.value;
		offset[2] = vr_leg_holster_model_z_offset.value;
		XR_ModModel ("progs/legholster.mdl", scale, offset);
	}

	// Hands. Scale and origin move together so the model shrinks toward the
	// controller point rather than drifting off it: a vertex ends up at
	// v * scale + scale_origin, so scaling one without the other slides the
	// whole model. No scale correction here -- quakevr applies none to these
	// models, so vr_hand_scale is the final ratio and reads as one.
	{
		static const char *hand_models[6] = {"progs/hand_base.mdl",	  "progs/finger_thumb.mdl", "progs/finger_index.mdl",
											 "progs/finger_middle.mdl", "progs/finger_ring.mdl",  "progs/finger_pinky.mdl"};
		const float		   k = vr_hand_scale.value;
		size_t			   n;

		for (n = 0; n < countof (hand_models); n++)
		{
			qmodel_t   *m = Mod_ForName (hand_models[n], false);
			aliashdr_t *hh;
			int			j;

			if (!m || m->type != mod_alias)
				continue;
			hh = (aliashdr_t *)Mod_Extradata (m);
			if (!hh)
				continue;

			for (j = 0; j < 3; j++)
			{
				hh->scale[j] = hh->original_scale[j] * k;
				hh->scale_origin[j] = hh->original_scale_origin[j] * k;
			}
		}
	}

	VR_XR_ModAllWeapons ();
}

/*
VR_XR_ScaleDump_f

Prints what every VR model is actually being drawn at, so a complaint about
scale can be answered with numbers instead of another guess. "ratio" is the
factor against the model's own authored size: 1.0 is untouched.
===============
*/
/*
===============
VR_XR_BodyDump_f

Prints where the body actually is, so placement can be argued from numbers.
Everything is in Quake units relative to the player entity, except the scales.
===============
*/
static void VR_XR_BodyDump_f (void)
{
	static const int hs[6] = {QVR_HS_LEFT_SHOULDER_HOLSTER, QVR_HS_RIGHT_SHOULDER_HOLSTER, QVR_HS_LEFT_HIP_HOLSTER,
							  QVR_HS_RIGHT_HIP_HOLSTER,		QVR_HS_LEFT_UPPER_HOLSTER,	  QVR_HS_RIGHT_UPPER_HOLSTER};
	static const char *hn[6] = {"L shoulder", "R shoulder", "L hip", "R hip", "L upper", "R upper"};
	vec3_t			   po;
	int				   i;

	if (cl.viewentity <= 0 || cl.viewentity >= cl.max_edicts || !cl.entities)
	{
		Con_Printf ("no player\n");
		return;
	}
	VectorCopy (cl.entities[cl.viewentity].origin, po);

	Con_Printf ("world_scale %.3f -> %.2f units/m | floor_offset %.1f | height_cal %.3f\n", vr_world_scale.value, VR_METERS_TO_UNITS, vr_floor_offset.value,
				vr_height_calibration.value);
	Con_Printf ("head z %.1f (rel player) | crouch %.2f\n", xr_head_pos_valid ? xr_last_head_pos[2] : 0.0f, XR_CrouchRatio ());

	{
		float head_m = xr_head_pos_valid ? ((xr_last_head_pos[2] - vr_floor_offset.value) / VR_METERS_TO_UNITS) : 0.0f;
		Con_Printf ("torso: head %.3f m * mult %.1f + ofs %.1f = z %.1f (rel player)\n", head_m, vr_vrtorso_head_z_mult.value, vr_vrtorso_z_offset.value,
					cl.vrtorso.origin[2] - po[2]);

		if (cl.vrtorso.model && cl.vrtorso.model->type == mod_alias)
		{
			aliashdr_t *th = (aliashdr_t *)Mod_Extradata (cl.vrtorso.model);
			if (th)
				Con_Printf (
					"torso: scale %.3f %.3f %.3f  origin_z %.1f  spans z %.1f .. %.1f (rel player)\n", th->scale[0], th->scale[1], th->scale[2],
					th->scale_origin[2], cl.vrtorso.origin[2] - po[2] + th->scale_origin[2],
					cl.vrtorso.origin[2] - po[2] + th->scale_origin[2] + th->scale[2] * 255.0f);
		}
	}

	for (i = 0; i < 6; i++)
	{
		vec3_t s;
		if (VR_XR_HolsterSpot (hs[i], s))
			Con_Printf ("  %-11s x %6.1f  y %6.1f  z %6.1f\n", hn[i], s[0] - po[0], s[1] - po[1], s[2] - po[2]);
	}

	{
		qmodel_t *m = Mod_ForName ("progs/legholster.mdl", false);
		if (m && m->type == mod_alias)
		{
			aliashdr_t *h = (aliashdr_t *)Mod_Extradata (m);
			if (h)
				Con_Printf ("legholster: drawn size %.1f %.1f %.1f\n", h->scale[0] * 255.0f, h->scale[1] * 255.0f, h->scale[2] * 255.0f);
		}
	}
}

static void VR_XR_ScaleDump_f (void)
{
	static const char *names[] = {
		"progs/hand_base.mdl", "progs/hand.mdl", "progs/finger_thumb.mdl", "progs/finger_index.mdl", "progs/finger_middle.mdl",
		"progs/finger_ring.mdl", "progs/finger_pinky.mdl", "progs/vrtorso.mdl", "progs/legholster.mdl", "progs/v_shot.mdl",
		"progs/v_shot2.mdl", "progs/v_axe.mdl", "progs/v_nail.mdl", "progs/v_rock2.mdl", "progs/v_light.mdl"};
	size_t i;

	Con_Printf ("world_scale %.3f  gunmodelscale %.3f  ->  scale_correct %.4f\n", vr_world_scale.value, vr_gunmodelscale.value,
				VR_XR_GetScaleCorrect ());
	Con_Printf ("wpn_offsets %s\n", vr_wpn_offsets.value ? "ON" : "off (position only; scale always applies)");

	for (i = 0; i < countof (names); i++)
	{
		qmodel_t   *model = Mod_ForName (names[i], false);
		aliashdr_t *hdr;
		float		ratio;

		if (!model || model->type != mod_alias)
		{
			Con_Printf ("  %-24s MISSING\n", names[i]);
			continue;
		}

		hdr = (aliashdr_t *)Mod_Extradata (model);
		if (!hdr)
			continue;

		ratio = (hdr->original_scale[0] != 0.0f) ? hdr->scale[0] / hdr->original_scale[0] : 0.0f;
		Con_Printf ("  %-24s ratio %6.3f  ofs %7.2f %7.2f %7.2f\n", names[i], ratio, hdr->scale_origin[0], hdr->scale_origin[1], hdr->scale_origin[2]);
	}
}

void VR_XR_ModAllWeapons (void)
{
	const vr_wpn_offset_t *table;
	size_t				   count, i;

	if (!vr_xr_active)
		return;

	if (!strcmp (COM_SkipPath (com_gamedir), "ad"))
	{
		table = vr_wpn_offsets_ad;
		count = countof (vr_wpn_offsets_ad);
	}
	else
	{
		table = vr_wpn_offsets_id1;
		count = countof (vr_wpn_offsets_id1);
	}

	for (i = 0; i < count; i++)
	{
		qmodel_t   *model;
		aliashdr_t *hdr;

		// false: a weapon the current game does not ship is not an error
		model = Mod_ForName (table[i].model, false);
		if (!model || model->type != mod_alias)
			continue;

		hdr = (aliashdr_t *)Mod_Extradata (model);
		if (hdr)
			VR_XR_ApplyWeaponModelMod (hdr, table[i].model);
	}
}

/*
XR_TwoHandedAim

Returns true and fills out_angles when the off hand is gripping close enough to
the main hand to count as supporting the weapon.
===============
*/
static qboolean XR_TwoHandedAim (int main_hand, vec3_t out_angles)
{
	static float aim_transition = 0.0f;	  // 0 one-handed, 1 fully two-handed
	static float stock_transition = 0.0f; // 0 hands only, 1 shouldered

	const int off_hand = main_hand ^ 1;
	vec3_t	  hand_diff, hand_dir, shoulder, shoulder_diff, avg_diff, avg_dir;
	vec3_t	  orig_dir, right, up, stock_dir, mix_dir;
	float	  len, dot, speed;
	qboolean  aiming, use_stock;
	int		  i;

	if (!vr_two_handed.value || vr_2h_mode.value == 0)
		return false;
	if (!vr_xr_hand[main_hand].tracked || !vr_xr_hand[off_hand].tracked)
		return false;

	// the barrel runs from the holding hand out to the supporting one
	VectorSubtract (vr_xr_hand[off_hand].pos, vr_xr_hand[main_hand].pos, hand_diff);
	len = VectorLength (hand_diff);
	if (len < 0.001f)
		return false;
	VectorScale (hand_diff, 1.0f / len, hand_dir);

	// Where the weapon already points, which is what the blend starts from.
	AngleVectors (vr_xr_hand[main_hand].aim_angles, orig_dir, right, up);

	// quakevr's dynamic grab distance, vr.cpp VR_GoodDistanceForDynamic2HGrabImpl
	aiming = (vr_xr_hand[off_hand].grip >= 0.5f) && (len > 5.0f) && (len < 25.0f);

	// and its angle gate: the supporting hand has to be roughly out along the
	// barrel, not off to one side holding something else
	dot = DotProduct (hand_dir, orig_dir);
	if (!vr_2h_disable_angle_threshold.value && dot <= vr_2h_angle_threshold.value)
		aiming = false;

	// The virtual stock. With the weapon drawn back near the shoulder, aiming
	// along shoulder-to-hand is steadier than hand-to-hand, and quakevr mixes
	// the two rather than switching (VR_Get2HVirtualStockMix).
	shoulder[0] = xr_last_head_pos[0] + vr_shoulder_offset_x.value;
	shoulder[1] = xr_last_head_pos[1] + (main_hand == VR_XR_MainHand () ? -vr_shoulder_offset_y.value : vr_shoulder_offset_y.value);
	shoulder[2] = xr_last_head_pos[2] - vr_shoulder_offset_z.value;

	VectorSubtract (vr_xr_hand[off_hand].pos, shoulder, shoulder_diff);

	{
		vec3_t to_shoulder;
		VectorSubtract (vr_xr_hand[main_hand].pos, shoulder, to_shoulder);
		use_stock = (vr_2h_mode.value == 2) && (VectorLength (to_shoulder) < vr_virtual_stock_thresh.value);
	}

	for (i = 0; i < 3; i++)
		avg_diff[i] = hand_diff[i] + (shoulder_diff[i] - hand_diff[i]) * vr_2h_virtual_stock_factor.value;
	VectorNormalize (avg_diff);
	VectorCopy (avg_diff, avg_dir);

	// Both transitions ease rather than snap, at quakevr's rate of 5 per
	// second, so picking the weapon up with the second hand does not jerk the
	// aim across (transitionVar, vr.cpp:2960-2966).
	speed = (float)(cl.time - cl.oldtime) * 5.0f;
	aim_transition += aiming ? speed : -speed;
	aim_transition = CLAMP (0.0f, aim_transition, 1.0f);
	stock_transition += (use_stock && aiming) ? speed : -speed;
	stock_transition = CLAMP (0.0f, stock_transition, 1.0f);

	if (aim_transition <= 0.0f)
		return false;

	for (i = 0; i < 3; i++)
	{
		stock_dir[i] = hand_dir[i] + (avg_dir[i] - hand_dir[i]) * stock_transition;
		mix_dir[i] = orig_dir[i] + (stock_dir[i] - orig_dir[i]) * aim_transition;
	}
	VectorNormalize (mix_dir);

	// atan2 rather than asin, which saturates at straight up and straight down
	out_angles[YAW] = RAD2DEG (atan2f (mix_dir[1], mix_dir[0]));
	out_angles[PITCH] = -RAD2DEG (atan2f (mix_dir[2], sqrtf (mix_dir[0] * mix_dir[0] + mix_dir[1] * mix_dir[1])));
	// roll stays with the holding hand: two-handing a weapon steadies where it
	// points, it does not stop the wrist rotating it
	out_angles[ROLL] = vr_xr_hand[main_hand].aim_angles[ROLL];
	return true;
}

/*
VR_XR_HandSpeed
===============
*/
float VR_XR_HandSpeed (int hand)
{
	if (!xr_input_ready || hand < 0 || hand >= VR_HANDS)
		return 0.0f;
	return vr_xr_hand[hand].speed;
}

/*
VR_XR_WeaponPose

Puts the gun where the hand is. The hand pose is in play space, so it has to be
rotated by the body yaw and offset from the player's origin the same way the
view is.
===============
*/
qboolean VR_XR_WeaponPose (const vec3_t player_origin, vec3_t out_origin, vec3_t out_angles)
{
	int			hand;
	const vr_hand_t *h;
	float		yaw, s, c;
	vec3_t		local;

	if (!xr_input_ready || !vr_hand_aiming.value || !VR_XR_SessionRunning ())
		return false;

	hand = VR_XR_MainHand ();
	h = &vr_xr_hand[hand];
	if (!h->tracked)
		return false;

	// Diagnostic: report once per second whether the gun is actually being
	// placed at the hand, and where. Guessing at this from the headset has not
	// been productive.

	// Head-relative in XY, absolute in Z -- same basis as the camera and the QC
	// hand positions, so all three agree wherever the player stands.
	// Position from the grip pose, orientation from the aim pose.
	//
	// These are two different points on the controller. Grip is the centroid of
	// the palm, which is where the hand actually is and therefore where the
	// hand models are placed by XR_HandToWorld; aim is a pointing ray whose
	// origin sits forward of it, centimetres away on an Index controller. Using
	// aim for position too put the weapon that far outside the hand holding it.
	//
	// quakevr never has to make this choice: OpenVR exposes a single controller
	// pose and cl.handpos is the one position everything uses. Taking position
	// from grip is the faithful equivalent -- and aim is still the right source
	// for orientation, being the analogue of quakevr's pose plus vr_gunangle.
	local[0] = h->pos[0] - xr_last_head_pos[0] + vr_gun_offset_x.value;
	local[1] = h->pos[1] - xr_last_head_pos[1] + vr_gun_offset_y.value;
	local[2] = h->pos[2] + vr_gun_offset_z.value;
	yaw = DEG2RAD (cl.viewangles[YAW]);
	s = sinf (yaw);
	c = cosf (yaw);

	// Same base as the camera: player origin plus the tracked offset, with no
	// STAT_VIEWHEIGHT. The headset and controllers already report real heights.
	out_origin[0] = player_origin[0] + local[0] * c - local[1] * s;
	out_origin[1] = player_origin[1] + local[0] * s + local[1] * c;
	out_origin[2] = player_origin[2] + local[2];

	// Aim pose, for position and orientation both.
	//
	// quakevr uses a single controller orientation for everything (vr.cpp:2779)
	// and corrects it with vr_gunangle, because OpenVR exposes one pose. OpenXR
	// exposes two with different conventions: grip is the controller's own
	// frame -- on a Touch controller its forward axis runs along the forearm,
	// which points the weapon back at the player -- while aim is defined as the
	// pointing ray. Aim is therefore the correct analogue, and using it for
	// position as well keeps everything in one frame, so the model offset is
	// not swung by a grip/aim frame difference as the wrist turns.
	//
	// vr_gunangle consequently defaults to 0 here: the aim pose already
	// incorporates the correction quakevr's 32 degrees was applying by hand.
	VectorCopy (h->aim_angles, out_angles);
	out_angles[YAW] += cl.viewangles[YAW];
	out_angles[PITCH] -= vr_gunangle.value;
	out_angles[YAW] += vr_gunyaw.value;
	out_angles[ROLL] += vr_gunroll.value;

	// the off hand can be angled apart from the main one (quakevr vr.cpp:3141)
	if (h == &vr_xr_hand[VR_XR_OffHand ()])
	{
		out_angles[PITCH] += vr_offhandpitch.value;
		out_angles[YAW] += vr_offhandyaw.value;
	}

	// Keep the barrel out of walls before the pitch is flipped for drawing,
	// since the collision sweep needs a real direction.
	VR_XR_ResolveGunCollision (out_origin, out_angles, 24.0f);

	// A light exactly at the computed hand position. If this sits on your
	// controller then the position is right and any remaining error is in the
	// model offsets; if it does not, the error is in the position maths.
	if (vr_gun_debug.value && cls.state == ca_connected && cl.worldmodel)
	{
		dlight_t *dl = CL_AllocDlight (-2);
		if (dl)
		{
			VectorCopy (out_origin, dl->origin);
			dl->radius = 40.0f;
			dl->die = cl.time + 0.05f;
			dl->decay = 0.0f;
			dl->color[0] = 0.2f;
			dl->color[1] = 0.4f;
			dl->color[2] = 1.0f;
		}
	}

	// viewmodels are drawn with inverted pitch, matching CalcGunAngle
	out_angles[PITCH] = -out_angles[PITCH];

	// Model-only pitch, added after the flip the way quakevr does it:
	// angles[PITCH] = -handrot[PITCH] + oPitch (view.cpp:722). It moves the
	// drawn weapon without touching the direction the shot travels.
	out_angles[PITCH] += vr_gunmodelpitch.value;
	return true;
}

/*
VR_XR_AimAngles

The aim pose is the controller's pointing ray, which is what the runtime
intends for weapons -- distinct from the grip pose, which is where the hand
physically sits.
===============
*/
qboolean VR_XR_AimAngles (vec3_t out_angles)
{
	int hand;

	if (!xr_input_ready || !vr_hand_aiming.value || !VR_XR_SessionRunning ())
		return false;

	hand = VR_XR_MainHand ();
	if (!vr_xr_hand[hand].tracked)
		return false;

	if (!XR_TwoHandedAim (hand, out_angles))
		VectorCopy (vr_xr_hand[hand].aim_angles, out_angles);

	// The same frame correction the drawn models get, so the shot travels where
	// the weapon points instead of 30 degrees above it.
	//
	// This started out applied only to the drawn path, on the theory that the
	// OpenXR aim pose was already a true pointing ray and needed no correction.
	// It is not: measured on this hardware the aim pose sits about 30 degrees
	// above where the controller is actually pointed, which is why models and
	// hitscans were both high and appeared to agree with each other. quakevr
	// never splits the two -- vr_gunangle goes into handrot, which drives the
	// visible gun and the aim direction alike (vr.cpp:2840, view.cpp:723) --
	// and that is what this restores.
	out_angles[PITCH] -= vr_gunangle.value;
	out_angles[YAW] += vr_gunyaw.value;
	out_angles[ROLL] += vr_gunroll.value;

	// hand angles are in play space; the body yaw the player has turned to with
	// the stick sits underneath, exactly as it does for the head
	out_angles[YAW] += cl.viewangles[YAW];
	return true;
}

/*
VR_XR_UpdateLaser

A dynamic light at the impact point. Cheap, needs no new rendering path, and
reads as a laser dot because it lights the surface it lands on.
===============
*/
/*
===============
VR_XR_AutosaveTick

quakevr rotates through a ring of autosave slots rather than overwriting one,
so a bad save cannot cost the run (saveutil.cpp:190-235). Twelve slots, matching
its MAX_AUTOSAVES, named auto0 through auto11.

quakevr picks the oldest slot by reading each file's timestamp. Cycling through
them in order comes to the same thing -- the slot next in line is always the one
written longest ago -- without needing to stat the save directory.

Only in a single-player game that is actually running, with the player alive:
saving during a demo, a menu, an intermission or a death would produce a save
nobody wants.
===============
*/
/*
===============
VR_XR_DebugPrints

quakevr's velocity readouts (vr.cpp debugPrintHandvel and its head
counterpart). Rate-limited to once a second: printed every frame they scroll
far too fast to read anything off.
===============
*/
void VR_XR_DebugPrints (void)
{
	static double last_print = 0.0;

	if (!VR_XR_SessionRunning ())
		return;
	if (!vr_debug_print_handvel.value && !vr_debug_print_headvel.value)
		return;
	if (realtime - last_print <= 1.0)
		return;

	last_print = realtime;

	if (vr_debug_print_handvel.value)
		Con_Printf (
			"handvel: L %.2f (%.1f %.1f %.1f) | R %.2f (%.1f %.1f %.1f)\n", vr_xr_hand[0].speed, vr_xr_hand[0].velocity[0], vr_xr_hand[0].velocity[1],
			vr_xr_hand[0].velocity[2], vr_xr_hand[1].speed, vr_xr_hand[1].velocity[0], vr_xr_hand[1].velocity[1], vr_xr_hand[1].velocity[2]);

	if (vr_debug_print_headvel.value && xr_head_vel_valid)
		Con_Printf ("headvel: (%.1f %.1f %.1f)\n", xr_head_velocity[0], xr_head_velocity[1], xr_head_velocity[2]);
}

void VR_XR_AutosaveTick (void)
{
	static double last_save = 0.0;
	static int	  slot = 0;

	if (vr_autosave_seconds.value <= 0.0f)
		return;
	if (!sv.active || svs.maxclients != 1)
		return;
	if (cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback)
		return;
	if (cl.intermission || cl.stats[STAT_HEALTH] <= 0)
		return;

	// start the clock on the first eligible frame rather than at time zero,
	// which would fire a save the instant a map loads
	if (last_save == 0.0)
	{
		last_save = realtime;
		return;
	}

	if (realtime - last_save < vr_autosave_seconds.value)
		return;

	last_save = realtime;

	if (vr_autosave_show_message.value)
		Con_Printf ("Creating autosave...\n");

	Cbuf_AddText (va ("save auto%d\n", slot));
	slot = (slot + 1) % 12;
}

void VR_XR_UpdateLaser (void)
{
	vec3_t	  origin, angles, forward, right, up, end, impact;
	dlight_t *dl;

	// Every one of these has to hold before tracing. cls.state goes to
	// ca_connected as soon as the connection is up, which is well before the
	// map exists -- and TraceLine dereferences cl.worldmodel->hulls with no
	// null check of its own, so running a frame early is an outright crash.
	if (!vr_laser.value)
		return;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		return;
	if (!cl.worldmodel || cl.intermission)
		return;
	if (cl.viewentity <= 0 || cl.viewentity >= cl.max_edicts || !cl.entities)
		return;

	if (!VR_XR_WeaponPose (cl.entities[cl.viewentity].origin, origin, angles))
		return;

	// the viewmodel is drawn with inverted pitch, so undo that to get a real
	// direction back out
	angles[PITCH] = -angles[PITCH];
	AngleVectors (angles, forward, right, up);

	VectorMA (origin, 8192.0f, forward, end);
	TraceLine (origin, end, impact);
	if (VectorLength (impact) == 0.0f)
		return; // hit nothing worth marking

	dl = CL_AllocDlight (0);
	if (!dl)
		return;

	VectorCopy (impact, dl->origin);
	dl->radius = 32.0f;
	dl->die = cl.time + 0.05f; // one frame; re-placed every frame
	dl->decay = 0.0f;
	dl->color[0] = 1.0f;
	dl->color[1] = 0.15f;
	dl->color[2] = 0.1f;
}

/*
XR_UpdateRoomscale

Called once per frame after the views are located. Converts however far the
player physically moved into a delta the movement command can carry.
===============
*/
static void XR_UpdateRoomscale (void)
{
	vec3_t head;
	float  scale = VR_METERS_TO_UNITS;

	xr_roomscale_delta[0] = xr_roomscale_delta[1] = xr_roomscale_delta[2] = 0.0f;

	if (!xr_views_valid)
	{
		xr_head_pos_valid = false;
		return;
	}

	// midpoint of the two eyes is close enough to the head for this purpose
	head[0] = -0.5f * (xr_views[0].pose.position.z + xr_views[1].pose.position.z) * scale;
	head[1] = -0.5f * (xr_views[0].pose.position.x + xr_views[1].pose.position.x) * scale;
	head[2] = 0.5f * (xr_views[0].pose.position.y + xr_views[1].pose.position.y) * scale;

	{
		vec3_t head_angles;
		XR_QuatToQuakeAngles (&xr_views[0].pose.orientation, head_angles);
		xr_head_yaw = head_angles[YAW];
	}

	if (!xr_head_pos_valid)
	{
		VectorCopy (head, xr_last_head_pos);
		xr_head_pos_valid = true;
		return;
	}

	if (vr_roomscale.value)
	{
		// horizontal only: crouching should not launch the player sideways
		xr_roomscale_delta[0] = (head[0] - xr_last_head_pos[0]) * vr_roomscale_mult.value;
		xr_roomscale_delta[1] = (head[1] - xr_last_head_pos[1]) * vr_roomscale_mult.value;
		xr_roomscale_delta[2] = 0.0f;

		// Nothing accumulates any more. The camera and hands are measured from
		// the head rather than from the play-space origin, so the delta only
		// ever needs to drive the player's movement -- there is no second copy
		// of it to cancel out, and therefore nothing that can drift.
	}

	VectorCopy (head, xr_last_head_pos);
}

/*
XR_ApplyDeadzone

Rescales past the deadzone so control stays smooth from the edge of the
deadzone outward, rather than jumping to a step.
===============
*/
static float XR_ApplyDeadzone (float v)
{
	const float dz = CLAMP (0.0f, vr_deadzone.value, 0.9f);
	float		mag = fabsf (v);

	if (mag <= dz)
		return 0.0f;
	mag = (mag - dz) / (1.0f - dz);
	return v < 0.0f ? -mag : mag;
}

/*
VR_XR_AdjustAngles

Right stick turns the body. Snap turning is the default because smooth turning
is the single most reliable way to make people motion sick.
===============
*/
void VR_XR_AdjustAngles (void)
{
	static qboolean snap_ready = true;
	float			x;

	if (!xr_input_ready || !VR_XR_SessionRunning ())
		return;

	// quakevr gates stick turning behind this so a player can commit to real
	// turning without the stick fighting them (vr.cpp:4632)
	if (vr_enable_joystick_turn.value != 1)
		return;

	x = XR_ApplyDeadzone (vr_xr_hand[VR_HAND_RIGHT].stick[0]);

	if (vr_snap_turn.value > 0.0f)
	{
		// one turn per push; the stick has to return near centre to re-arm
		if (fabsf (x) < 0.5f)
			snap_ready = true;
		else if (snap_ready)
		{
			snap_ready = false;
			cl.viewangles[YAW] -= (x > 0.0f ? vr_snap_turn.value : -vr_snap_turn.value);
		}
	}
	else
	{
		cl.viewangles[YAW] -= x * vr_turn_speed.value * host_frametime;
	}
}

/*
VR_XR_Move
===============
*/
void VR_XR_Move (usercmd_t *cmd)
{
	float fwd, side;

	if (!xr_input_ready || !VR_XR_SessionRunning ())
		return;

	fwd = XR_ApplyDeadzone (vr_xr_hand[VR_HAND_LEFT].stick[1]);
	side = XR_ApplyDeadzone (vr_xr_hand[VR_HAND_LEFT].stick[0]);

	// added, not assigned: keyboard input stays live alongside the controller
	cmd->forwardmove += cl_forwardspeed.value * fwd;
	cmd->sidemove += cl_sidespeed.value * side;

	// grip on the left hand doubles as run, mirroring the speed key
	if (vr_xr_hand[VR_HAND_LEFT].grip > 0.7f)
	{
		cmd->forwardmove *= cl_movespeedkey.value;
		cmd->sidemove *= cl_movespeedkey.value;
	}

	// Room-scale: physical walking, expressed as movement input. The delta is a
	// distance, so divide by frametime to get the speed that covers it, and
	// rotate world-space motion into the player's facing.
	if (vr_roomscale.value && host_frametime > 0.0)
	{
		const float yaw = DEG2RAD (cl.viewangles[YAW]);
		const float s = sinf (yaw), c = cosf (yaw);
		const float inv_dt = (float)(1.0 / host_frametime);
		const float wx = xr_roomscale_delta[0] * inv_dt;
		const float wy = xr_roomscale_delta[1] * inv_dt;

		cmd->forwardmove += wx * c + wy * s;
		cmd->sidemove += -wx * s + wy * c;
	}

	// The server builds its movement basis from the angles we send it, and with
	// hand aiming those are the controller's, not the head's. Counter-rotate by
	// the difference so pushing the stick forward still walks where the player
	// is looking rather than where they happen to be pointing the gun.
	{
		vec3_t aim;
		if (VR_XR_AimAngles (aim))
		{
			const float delta = DEG2RAD (xr_head_yaw + cl.viewangles[YAW] - aim[YAW]);
			const float s = sinf (delta), c = cosf (delta);
			const float f = cmd->forwardmove, r = cmd->sidemove;

			cmd->forwardmove = f * c - r * s;
			cmd->sidemove = f * s + r * c;
		}
	}
}

/*
VR_XR_Buttons
===============
*/
unsigned int VR_XR_Buttons (void)
{
	static qboolean was_firing = false;
	static qboolean melee_armed = true;
	static int		last_health = 0;

	unsigned int bits = 0;
	int			 aim = VR_XR_MainHand ();
	qboolean	 firing;

	if (!xr_input_ready || !VR_XR_SessionRunning ())
		return 0;

	firing = vr_xr_hand[aim].trigger > 0.5f;

	// Melee: swing the aiming hand hard enough and it counts as an attack.
	// Genuinely swung, not merely moved -- the threshold has to be high enough
	// that walking around does not flail the axe. quakevr does the same, gated
	// on handvelmag against vr_melee_threshold.
	if (vr_melee_threshold.value > 0.0f)
	{
		const float speed = vr_xr_hand[aim].speed;
		if (speed < vr_melee_threshold.value * 0.5f)
			melee_armed = true; // must slow down before another swing counts
		else if (speed > vr_melee_threshold.value && melee_armed)
		{
			melee_armed = false;
			firing = true;
			if (vr_haptics.value)
				VR_XR_Haptic (aim, 0.06f, 180.0f, 0.6f);
		}
	}

	if (firing)
		bits |= 1; // attack
	if (vr_xr_hand[VR_HAND_RIGHT].btn_a || vr_xr_hand[VR_HAND_LEFT].btn_a)
		bits |= 2; // jump

	// Room-scale jump: physically jump and the player jumps.
	//
	// quakevr gates it on both the head rising fast enough and the head being
	// above the calibrated standing height (vr.cpp:4173-4175), so that standing
	// up out of a crouch does not read as a jump. The velocity is in metres per
	// second, straight from the runtime, which is what the threshold is in.
	if (vr_roomscale_jump.value && xr_head_vel_valid)
	{
		// Both sides in metres. XR_LocateHand scales velocity into Quake units
		// on the way out, and the stored head height carries vr_floor_offset,
		// so both are converted back -- vr_height_calibration is a real
		// standing height in metres and means nothing in Quake units.
		const float rise = xr_head_velocity[2] / VR_METERS_TO_UNITS;
		const float height = (xr_last_head_pos[2] - vr_floor_offset.value) / VR_METERS_TO_UNITS;
		if (rise > vr_roomscale_jump_threshold.value && height > vr_height_calibration.value)
			bits |= 2;
	}


	if (vr_haptics.value)
	{
		// a short kick as the shot goes off, not continuously while held
		if (firing && !was_firing)
			VR_XR_Haptic (aim, 0.05f, 160.0f, 0.75f);

		// and a heavier one on both hands when the player takes damage
		if (cl.stats[STAT_HEALTH] < last_health && last_health > 0)
		{
			VR_XR_Haptic (VR_HAND_LEFT, 0.15f, 90.0f, 0.9f);
			VR_XR_Haptic (VR_HAND_RIGHT, 0.15f, 90.0f, 0.9f);
		}
	}
	last_health = cl.stats[STAT_HEALTH];
	was_firing = firing;

	return bits;
}

/*
VR_XR_Impulse

Off-hand stick left/right cycles weapons. Edge triggered: one weapon per push,
and the stick has to return near centre before it fires again.
===============
*/
int VR_XR_Impulse (void)
{
	static qboolean armed = true;
	const int		off_hand = VR_XR_OffHand ();
	float			x;

	if (!xr_input_ready || !VR_XR_SessionRunning ())
		return 0;

	x = vr_xr_hand[off_hand].stick[0];

	if (fabsf (x) < 0.4f)
	{
		armed = true;
		return 0;
	}

	if (!armed)
		return 0;
	armed = false;

	if (vr_haptics.value)
		VR_XR_Haptic (off_hand, 0.04f, 200.0f, 0.4f);

	return x > 0.0f ? 10 : 12; // impulse 10 = next weapon, 12 = previous
}

/*
VR_XR_Calibrate_f

Sets vr_floor_offset from where the player's head actually is, so the in-game
eye height matches theirs instead of assuming a standard body.
===============
*/
static void VR_XR_Calibrate_f (void)
{
	float measured, target;

	if (!VR_XR_SessionRunning () || !xr_head_pos_valid)
	{
		Con_Printf ("vr_calibrate: no headset tracking\n");
		return;
	}

	// current head height above the play space floor, in Quake units
	measured = xr_last_head_pos[2];
	// where Quake wants the eye to be relative to the player origin
	target = (float)DEFAULT_VIEWHEIGHT;

	Cvar_SetValue ("vr_floor_offset", target - measured);
	Con_Printf ("vr_calibrate: head at %.1f units, floor offset now %.1f\n", measured, target - measured);
}

/*
VR_XR_Recenter_f

Forgets the room-scale movement consumed so far, which re-anchors the player to
wherever they are physically standing now.
===============
*/
static void VR_XR_Recenter_f (void)
{
	// nothing to reset: room-scale no longer accumulates
	xr_head_pos_valid = false;
	Con_Printf ("vr_recenter: play space re-anchored\n");
}

/*
	QUAKEC FIELDS

	Publishes hand state onto the player's edict so game logic can read it as
	ordinary entity fields. Field names and semantics are quakevr's
	(QC/vr_sys_fields.qc), and the values are written in the same places
	quakevr writes them (sv_user.cpp:650-699).

	The difference is how: quakevr welds the fields into entvars_t at fixed
	offsets, which is what forces its progs.dat CRC and locks out every other
	mod. Here they resolve by name through qcvm->extfields, so a progs.dat that
	does not declare them simply reports -1 and the writes are skipped.

	Only the local player is served for now. Carrying this to a remote client
	needs the extended clc_move quakevr uses (protocol.hpp:554-589), which is a
	protocol change and a separate piece of work.

================================================================================
*/

/*
XR_SetFloat / XR_SetVector
===============
*/
static void XR_SetFloat (edict_t *ed, int ofs, float v)
{
	eval_t *val;
	if (ofs < 0)
		return; // progs.dat does not declare this field
	val = GetEdictFieldValue (ed, ofs);
	if (val)
		val->_float = v;
}

static void XR_SetVector (edict_t *ed, int ofs, const vec3_t v)
{
	eval_t *val;
	if (ofs < 0)
		return;
	val = GetEdictFieldValue (ed, ofs);
	if (val)
	{
		val->vector[0] = v[0];
		val->vector[1] = v[1];
		val->vector[2] = v[2];
	}
}

/*
XR_HandToWorld

Hand state is tracked in play space. Game logic wants it in the world, so it
gets the same treatment the view and the gun get: drop the room-scale movement
already consumed, rotate by the body yaw, offset from the player.
===============
*/
/*
===============
XR_HandVelToWorld

Rotates a play-space velocity into the world by the player's body yaw. Position
needs the extra offset work; a velocity only needs the rotation.
===============
*/
/*
===============
XR_ThrowVelocity

quakevr has two ways of working out how fast something leaves the hand
(vr.cpp:1984-2010).

Algorithm 0 uses the averaged hand velocity, which is what a straight push
gives. Algorithm 1 adds the wrist's spin: an object held a little above the
palm is carried by rotation as well as by translation, so the angular velocity
crossed with that offset contributes. It is the difference between shoving a
grenade and flicking one.
===============
*/
static void XR_ThrowVelocity (int hand, vec3_t out)
{
	const vr_hand_t *h = &vr_xr_hand[hand];
	vec3_t			 fwd, right, up, obj_offset, spin;

	VectorCopy (h->throw_velocity, out);

	if ((int)vr_throw_algorithm.value != 1)
		return;

	// where the thrown thing sits relative to the palm, up from it
	{
		vec3_t ang;
		VectorCopy (h->aim_angles, ang);
		AngleVectors (ang, fwd, right, up);
	}
	VectorNormalize (up);
	VectorScale (up, vr_throw_up_center_of_mass.value, obj_offset);

	{
		vec3_t avel;
		VectorCopy (h->angular_velocity, avel);
		CrossProduct (avel, obj_offset, spin);
	}
	VectorAdd (out, spin, out);
}

static void XR_HandVelToWorld (const vr_hand_t *h, const vec3_t in, vec3_t out)
{
	const float yaw = DEG2RAD (cl.viewangles[YAW]);
	const float s = sinf (yaw), c = cosf (yaw);

	(void)h;
	out[0] = in[0] * c - in[1] * s;
	out[1] = in[0] * s + in[1] * c;
	out[2] = in[2];
}

static void XR_HandToWorld (const vr_hand_t *h, const vec3_t player_origin, vec3_t out_pos, vec3_t out_angles, vec3_t out_vel)
{
	const float yaw = DEG2RAD (cl.viewangles[YAW]);
	const float s = sinf (yaw), c = cosf (yaw);
	vec3_t		local;

	// Head-relative in XY, absolute in Z -- the same shape quakevr stores its
	// controller positions in (vr.cpp:2760-2767). Measuring the hand from the
	// head rather than from the play-space origin is what keeps it attached to
	// the player no matter where they stand in the room.
	local[0] = h->pos[0] - xr_last_head_pos[0];
	local[1] = h->pos[1] - xr_last_head_pos[1];
	local[2] = h->pos[2];

	// same base as the camera and the gun -- no STAT_VIEWHEIGHT
	out_pos[0] = player_origin[0] + local[0] * c - local[1] * s;
	out_pos[1] = player_origin[1] + local[0] * s + local[1] * c;
	out_pos[2] = player_origin[2] + local[2];

	// Identical rotation to the drawn weapon, gun pre-rotation included.
	// quakevr uses one handrot for the visible gun, the aim direction and the
	// QC (view.cpp:723, vr.cpp:2840, 2857); publishing a different one here
	// would put holster geometry and muzzle direction out of step with what the
	// player can see.
	VectorCopy (h->aim_angles, out_angles);
	out_angles[YAW] += cl.viewangles[YAW];
	out_angles[PITCH] -= vr_gunangle.value;
	out_angles[YAW] += vr_gunyaw.value;
	out_angles[ROLL] += vr_gunroll.value;

	out_vel[0] = h->velocity[0] * c - h->velocity[1] * s;
	out_vel[1] = h->velocity[0] * s + h->velocity[1] * c;
	out_vel[2] = h->velocity[2];
}

/*
VR_XR_WriteEdictFields
===============
*/
/*
===============
XR_UpdateFlickReload

Spin the wrist hard enough and the weapon reloads -- the break-action flick a
shotgun gets in quakevr (vr.cpp:3195-3245).

Two conditions, both quakevr's. The wrist has to be turning faster than
vr_spinreload_x_angular_threshold, and the hand has to have come back round to
roughly where it started, dot over 0.6 against the direction it was pointing
when it was last still. The second is what separates a flick from simply waving
the weapon about: a flick returns.

quakevr also refuses unless the weapon is a super shotgun with a part-empty
clip, which it reads from stats its own protocol carries and this one does not.
That check belongs to the QuakeC here, which sees the same weapon and clip it
always did; the engine reports the gesture and the QC decides what it means.
===============
*/
static void XR_UpdateFlickReload (int hand, qboolean *curr, qboolean *prev)
{
	const vr_hand_t *h = &vr_xr_hand[hand];
	static vec3_t	 target_fwd[VR_HANDS];
	vec3_t			 ang, fwd, right, up;
	float			 avel_len;

	*prev = *curr;

	if (!h->tracked)
	{
		*curr = false;
		return;
	}

	avel_len = VectorLength (h->angular_velocity);

	VectorCopy (h->aim_angles, ang);
	AngleVectors (ang, fwd, right, up);

	// while the hand is near enough still, remember where it points; that is
	// what the flick has to come back to
	if (avel_len < 1.5f)
		VectorCopy (fwd, target_fwd[hand]);

	*curr = (avel_len >= vr_spinreload_x_angular_threshold.value) && (DotProduct (fwd, target_fwd[hand]) > 0.6f);
}

void VR_XR_WriteEdictFields (edict_t *ed)
{
	const struct pr_extfields_s *f;
	int							 main_hand, off_hand;
	vec3_t						 pos, ang, vel, origin;
	vec3_t						 head_v = {0.0f, 0.0f, 0.0f};

	if (!ed || !xr_input_ready || !VR_XR_SessionRunning ())
		return;
	if (!qcvm)
		return;

	f = &qcvm->extfields;
	VectorCopy (ed->v.origin, origin);

	main_hand = VR_XR_MainHand ();
	off_hand = main_hand ^ 1;

	// main hand
	XR_HandToWorld (&vr_xr_hand[main_hand], origin, pos, ang, vel);
	XR_SetVector (ed, f->handpos, pos);
	XR_SetVector (ed, f->handrot, ang);
	XR_SetVector (ed, f->handvel, vel);
	XR_SetFloat (ed, f->handvelmag, vr_xr_hand[main_hand].speed);
	{
		// averaged, then scaled -- the release frame alone under-reads a throw
		vec3_t tv;
		vec3_t rawtv;
		XR_ThrowVelocity (main_hand, rawtv);
		XR_HandVelToWorld (&vr_xr_hand[main_hand], rawtv, tv);
		VectorScale (tv, vr_weapon_throw_velocity_mult.value, tv);
		XR_SetVector (ed, f->handthrowvel, tv);
	}
	// the muzzle is the aim ray's origin, which is what weapons fire from
	XR_SetVector (ed, f->muzzlepos, pos);

	// off hand
	XR_HandToWorld (&vr_xr_hand[off_hand], origin, pos, ang, vel);
	XR_SetVector (ed, f->offhandpos, pos);
	XR_SetVector (ed, f->offhandrot, ang);
	XR_SetVector (ed, f->offhandvel, vel);
	XR_SetFloat (ed, f->offhandvelmag, vr_xr_hand[off_hand].speed);
	{
		vec3_t tv;
		XR_HandVelToWorld (&vr_xr_hand[off_hand], vr_xr_hand[off_hand].throw_velocity, tv);
		VectorScale (tv, vr_weapon_throw_velocity_mult.value, tv);
		XR_SetVector (ed, f->offhandthrowvel, tv);
	}
	XR_SetVector (ed, f->offmuzzlepos, pos);

	XR_SetVector (ed, f->headvel, head_v);
	XR_SetFloat (ed, f->vryaw, cl.viewangles[YAW]);

	// Which holster each hand is at, if any. This is the whole engine-side
	// contribution to holsters; the QC stores and swaps the weapons itself.
	{
		vec3_t hw, ha, hv;
		XR_HandToWorld (&vr_xr_hand[main_hand], origin, hw, ha, hv);
		XR_SetFloat (ed, f->mainhand_hotspot, (float)XR_ComputeHotSpot (hw, origin));
		XR_HandToWorld (&vr_xr_hand[off_hand], origin, hw, ha, hv);
		XR_SetFloat (ed, f->offhand_hotspot, (float)XR_ComputeHotSpot (hw, origin));
	}

	// Teleport destination, consumed once. quakevr latches it the same way
	// (vr.cpp:4449-4452) so the QC performs the move on exactly one frame.
	if (xr_send_teleport)
	{
		xr_send_teleport = false;
		XR_SetVector (ed, f->teleport_target, xr_teleport_impact);
	}

	// room-scale movement, as a velocity, exactly as quakevr sends it
	// (vr.cpp:4615-4617)
	if (host_frametime > 0.0)
	{
		vec3_t rs;
		const float inv_dt = (float)(1.0 / host_frametime);
		rs[0] = xr_roomscale_delta[0] * inv_dt;
		rs[1] = xr_roomscale_delta[1] * inv_dt;
		rs[2] = 0.0f;
		XR_SetVector (ed, f->roomscalemove, rs);
	}

	// vrbits0: quakevr's per-frame VR state bitfield (vr.cpp:4477-4499)
	{
		float bits = 0.0f;
		if (xr_teleporting)
			bits += (float)QVR_VRBITS0_TELEPORTING;
		if (vr_xr_hand[off_hand].grip > 0.5f)
			bits += (float)QVR_VRBITS0_OFFHAND_GRABBING;
		if (vr_xr_hand[main_hand].grip > 0.5f)
			bits += (float)QVR_VRBITS0_MAINHAND_GRABBING;
		{
			vec3_t unused;
			if (XR_TwoHandedAim (main_hand, unused))
				bits += (float)QVR_VRBITS0_2H_AIMING;
		}
		{
			static qboolean flick[VR_HANDS] = {false, false};
			static qboolean flick_prev[VR_HANDS] = {false, false};

			XR_UpdateFlickReload (main_hand, &flick[main_hand], &flick_prev[main_hand]);
			XR_UpdateFlickReload (off_hand, &flick[off_hand], &flick_prev[off_hand]);

			if (flick[main_hand])
				bits += (float)QVR_VRBITS0_MAINHAND_RELOADFLICKING;
			if (flick_prev[main_hand])
				bits += (float)QVR_VRBITS0_MAINHAND_PREVRELOADFLICKING;
			if (flick[off_hand])
				bits += (float)QVR_VRBITS0_OFFHAND_RELOADFLICKING;
			if (flick_prev[off_hand])
				bits += (float)QVR_VRBITS0_OFFHAND_PREVRELOADFLICKING;
		}

		XR_SetFloat (ed, f->vrbits0, bits);
	}

	XR_SetFloat (ed, f->ishuman, 1.0f);
}

/*
	TELEPORT LOCOMOTION

	Point with the off hand, hold to aim, release to go. quakevr traces a box
	from the player toward the hand's forward ray and only accepts the
	destination if the surface underfoot is close enough to level -- so you can
	teleport onto slopes and ledges, but not onto walls or ceilings.

	Ported from quakevr vr.cpp:2582-2631.

================================================================================
*/

/*
VR_XR_UpdateTeleport
===============
*/
void VR_XR_UpdateTeleport (void)
{
	const int off_hand = VR_XR_OffHand ();
	vec3_t	  mins, maxs, fwd, right, up, angles, start, target;
	trace_t	  trace;
	edict_t	 *player;
	qboolean  switched = false;
	qcvm_t   *old_vm = NULL;

	if (!vr_teleport_enabled.value || !xr_input_ready || !VR_XR_SessionRunning ())
		return;
	if (!sv.active || cls.state != ca_connected || cls.signon != SIGNONS)
		return;

	// same reason as VR_XR_ResolveGunCollision: EDICT_NUM and SV_Move act on
	// the current qcvm, and the host loop is not running the server's
	if (qcvm != &sv.qcvm)
	{
		old_vm = qcvm;
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);
		switched = true;
	}

	if (cl.viewentity <= 0 || cl.viewentity >= qcvm->num_edicts)
	{
		if (switched)
		{
			PR_SwitchQCVM (NULL);
			PR_SwitchQCVM (old_vm);
		}
		return;
	}

	player = EDICT_NUM (cl.viewentity);
	if (!player || player->free)
	{
		if (switched)
		{
			PR_SwitchQCVM (NULL);
			PR_SwitchQCVM (old_vm);
		}
		return;
	}

	// held: aim. quakevr uses the off hand so the weapon hand stays free.
	xr_teleporting = (vr_xr_hand[off_hand].btn_stick || vr_xr_hand[off_hand].trigger > 0.7f) ? true : false;

	if (xr_teleporting)
	{
		// the box quakevr sweeps: narrow, and a bit shorter than the player
		mins[0] = mins[1] = -6.0f;
		mins[2] = -12.0f;
		maxs[0] = maxs[1] = 6.0f;
		maxs[2] = 12.0f;

		VectorCopy (vr_xr_hand[off_hand].angles, angles);
		angles[YAW] += cl.viewangles[YAW];
		AngleVectors (angles, fwd, right, up);

		// start at the player, aim along the hand
		{
			vec3_t hand_world, hand_ang, hand_vel;
			XR_HandToWorld (&vr_xr_hand[off_hand], player->v.origin, hand_world, hand_ang, hand_vel);
			VectorCopy (hand_world, start);
		}
		VectorMA (start, vr_teleport_range.value, fwd, target);

		trace = SV_Move (start, mins, maxs, target, MOVE_NORMAL, player);

		// Slopes yes, walls and ceilings no: a floor points mostly straight up.
		xr_teleport_valid = (trace.fraction < 1.0f && trace.plane.normal[2] >= 0.75f && trace.plane.normal[2] <= 1.0f) ? true : false;

		VectorCopy (trace.endpos, xr_teleport_impact);
		xr_teleport_impact[2] += 12.0f; // lift clear of the surface

		// Mark the destination. quakevr spawns a particle effect here
		// (vr.cpp:2621); a dlight reads just as clearly and costs nothing,
		// and the colour tells you whether the spot is actually usable.
		if (cls.state == ca_connected && cl.worldmodel)
		{
			dlight_t *dl = CL_AllocDlight (-1);
			if (dl)
			{
				VectorCopy (trace.endpos, dl->origin);
				dl->origin[2] += 4.0f;
				dl->radius = 48.0f;
				dl->die = cl.time + 0.05f;
				dl->decay = 0.0f;
				if (xr_teleport_valid)
				{
					dl->color[0] = 0.2f;
					dl->color[1] = 1.0f;
					dl->color[2] = 0.3f;
				}
				else
				{
					dl->color[0] = 1.0f;
					dl->color[1] = 0.2f;
					dl->color[2] = 0.1f;
				}
			}
		}
	}
	else if (xr_was_teleporting && xr_teleport_valid)
	{
		// released over somewhere valid: go
		xr_send_teleport = true;
		if (vr_haptics.value)
			VR_XR_Haptic (off_hand, 0.08f, 140.0f, 0.7f);
	}

	xr_was_teleporting = xr_teleporting;

	if (switched)
	{
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (old_vm);
	}
}

/*
	GUN WALL COLLISIONS

	Without this the weapon happily pokes through walls, which both looks wrong
	and lets the player shoot around corners from cover.

	quakevr sweeps a small box from the hand to the muzzle and, on a hit, pulls
	the hand back so the muzzle sits at the impact instead. The gun stays in
	front of the wall and the player's real hand simply diverges from the
	virtual one, which is the standard resolution and reads far better than
	letting the barrel clip through. (quakevr vr.cpp:1765-1807)

================================================================================
*/

// Default off until the gun is confirmed sitting correctly in the hand: this
// runs a trace from the render path every frame, so if it misbehaves it looks
// exactly like the weapon not being attached at all.
cvar_t vr_gun_wall_collision = {"vr_gun_wall_collision", "0", CVAR_ARCHIVE};


static qboolean xr_gun_colliding = false;

/*
VR_XR_ResolveGunCollision

hand_pos is adjusted in place. muzzle_len is how far ahead of the hand the
barrel ends, in Quake units.
===============
*/
void VR_XR_ResolveGunCollision (vec3_t hand_pos, const vec3_t hand_angles, float muzzle_len)
{
	vec3_t	 mins, maxs, fwd, right, up, muzzle, local, ang;
	trace_t	 trace;
	edict_t *player;
	int		 i;
	qboolean switched = false;
	qcvm_t  *old_vm = NULL;

	xr_gun_colliding = false;

	if (!vr_gun_wall_collision.value || !VR_XR_SessionRunning ())
		return;
	if (!sv.active || cls.signon != SIGNONS)
		return;

	// EDICT_NUM and SV_Move both work against whatever qcvm is current, and
	// this is called from the client's view code where it is not the server's.
	// Reading the wrong VM's edict array hands back a garbage pointer.
	if (qcvm != &sv.qcvm)
	{
		old_vm = qcvm;
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);
		switched = true;
	}

	if (cl.viewentity <= 0 || cl.viewentity >= qcvm->num_edicts)
	{
		if (switched)
		{
			PR_SwitchQCVM (NULL);
			PR_SwitchQCVM (old_vm);
		}
		return;
	}

	player = EDICT_NUM (cl.viewentity);
	if (!player || player->free)
	{
		if (switched)
		{
			PR_SwitchQCVM (NULL);
			PR_SwitchQCVM (old_vm);
		}
		return;
	}

	mins[0] = mins[1] = mins[2] = -1.0f;
	maxs[0] = maxs[1] = maxs[2] = 1.0f;

	// AngleVectors takes a non-const vec3_t
	VectorCopy (hand_angles, ang);
	AngleVectors (ang, fwd, right, up);
	VectorScale (fwd, muzzle_len, local);
	VectorAdd (hand_pos, local, muzzle);

	trace = SV_Move (hand_pos, mins, maxs, muzzle, MOVE_NORMAL, player);

	// startsolid/allsolid mean the hand itself is already inside geometry --
	// standing close to a wall, say. The trace result is meaningless there and
	// acting on it snaps the gun to the hand's origin, which reads as the
	// weapon flying around rather than being held.
	if (trace.fraction < 1.0f && !trace.startsolid && !trace.allsolid)
	{
		xr_gun_colliding = true;
		// pull the hand back by however far the muzzle was blocked
		for (i = 0; i < 3; i++)
			hand_pos[i] = trace.endpos[i] - local[i];
	}

	if (switched)
	{
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (old_vm);
	}
}

/*
	HOLSTER HOTSPOTS

	Holsters are places on the player's body: reach to your shoulder, hip or
	chest and grab. The engine's job is only to report which spot a hand is
	near; quakevr's QC does the rest, storing and swapping the weapons.

	Anchors are computed relative to the player's body yaw so they follow the
	player around rather than sitting at fixed world positions. Offsets and
	thresholds are quakevr's (vr_cvars.cpp:92-122).

================================================================================
*/

cvar_t vr_shoulder_offset_x = {"vr_shoulder_offset_x", "-1", CVAR_ARCHIVE};
cvar_t vr_shoulder_offset_y = {"vr_shoulder_offset_y", "1.75", CVAR_ARCHIVE};
cvar_t vr_shoulder_offset_z = {"vr_shoulder_offset_z", "16", CVAR_ARCHIVE};

cvar_t vr_hip_offset_x = {"vr_hip_offset_x", "-3.5", CVAR_ARCHIVE};
cvar_t vr_hip_offset_y = {"vr_hip_offset_y", "7.0", CVAR_ARCHIVE};
cvar_t vr_hip_offset_z = {"vr_hip_offset_z", "0", CVAR_ARCHIVE};
cvar_t vr_hip_holster_thresh = {"vr_hip_holster_thresh", "6.5", CVAR_ARCHIVE};

cvar_t vr_shoulder_holster_offset_x = {"vr_shoulder_holster_offset_x", "-0.5", CVAR_ARCHIVE};
cvar_t vr_shoulder_holster_offset_y = {"vr_shoulder_holster_offset_y", "2.25", CVAR_ARCHIVE};
cvar_t vr_shoulder_holster_offset_z = {"vr_shoulder_holster_offset_z", "-0.25", CVAR_ARCHIVE};
cvar_t vr_shoulder_holster_thresh = {"vr_shoulder_holster_thresh", "7.8", CVAR_ARCHIVE};

cvar_t vr_upper_holster_offset_x = {"vr_upper_holster_offset_x", "-4.25", CVAR_ARCHIVE};
cvar_t vr_upper_holster_offset_y = {"vr_upper_holster_offset_y", "7", CVAR_ARCHIVE};
cvar_t vr_upper_holster_offset_z = {"vr_upper_holster_offset_z", "8.5", CVAR_ARCHIVE};
cvar_t vr_upper_holster_thresh = {"vr_upper_holster_thresh", "6.5", CVAR_ARCHIVE};

/*
XR_CrouchRatio

How far down the player has physically crouched, 0 standing to 1 fully down.
quakevr's VR_GetCrouchRatio: calibrated standing height over current head
height, minus one.
===============
*/
static float XR_CrouchRatio (void)
{
	float curr;

	if (!xr_head_pos_valid)
		return 0.0f;

	// head height in metres, which is what vr_height_calibration is in
	curr = (xr_last_head_pos[2] - vr_floor_offset.value) / VR_METERS_TO_UNITS;
	if (curr <= 0.01f)
		return 0.0f;

	return CLAMP (0.0f, (vr_height_calibration.value / curr) - 1.0f, 1.0f);
}

/*
===============
XR_BodyAnchor

A point hung off the player's body, which is where every holster lives.
quakevr's VR_GetBodyAnchor, followed properly this time (vr.cpp):

  - the body origin sits 2 units above the player entity, and drops by 18 times
    the crouch ratio
  - the frame pitches down by 35 degrees times the crouch ratio, so holsters
    swing forward as the player folds over instead of staying upright and
    ending up in the floor
  - the vertical offset is scaled by vr_height_calibration, so a tall player's
    hips sit further from their eyes than a short one's

That last one was missing here, and it is not a small correction: at the
default calibration it scales every holster's height by 1.6.
===============
*/
static void XR_BodyAnchor (const vec3_t player_origin, float ox, float oy, float oz, vec3_t out)
{
	const float crouch = XR_CrouchRatio ();
	const float height_ratio = CLAMP (0.0f, crouch, 0.8f);
	vec3_t		ang, fwd, right, up, origin;

	// quakevr blends head and hand direction for body yaw; the view yaw stands
	// in for that until VR_GetBodyYawAngle is ported.
	ang[PITCH] = height_ratio * -35.0f;
	ang[YAW] = cl.viewangles[YAW];
	ang[ROLL] = 0.0f;
	AngleVectors (ang, fwd, right, up);

	VectorCopy (player_origin, origin);
	origin[2] += 2.0f;
	origin[2] -= crouch * 18.0f;

	out[0] = origin[0] + right[0] * oy + fwd[0] * ox + up[0] * vr_height_calibration.value * oz;
	out[1] = origin[1] + right[1] * oy + fwd[1] * ox + up[1] * vr_height_calibration.value * oz;
	out[2] = origin[2] + right[2] * oy + fwd[2] * ox + up[2] * vr_height_calibration.value * oz;
}

/*
===============
XR_HolsterCrouchAdjust

Crouching slides the holsters forward along the body's facing, by a different
amount per set: quakevr's VR_GetHolsterXCrouchAdjustment multipliers are -9.5
for the hips, -1.5 for the uppers and 1.5 for the shoulders.
===============
*/
static void XR_HolsterCrouchAdjust (float mult, vec3_t out)
{
	const float height_ratio = CLAMP (0.0f, XR_CrouchRatio () - 0.2f, 0.6f);
	vec3_t		ang, fwd, right, up;

	ang[PITCH] = 0.0f;
	ang[YAW] = cl.viewangles[YAW];
	ang[ROLL] = 0.0f;
	AngleVectors (ang, fwd, right, up);

	VectorScale (fwd, height_ratio * mult, out);
}

/*
XR_ComputeHotSpot

Order matters and follows quakevr's (vr.cpp:4504-4553): shoulders, then hips,
then upper/chest. First match wins.
===============
*/
static int XR_ComputeHotSpot (const vec3_t hand_world, const vec3_t player_origin)
{
	static const int order[6] = {QVR_HS_LEFT_SHOULDER_HOLSTER, QVR_HS_RIGHT_SHOULDER_HOLSTER, QVR_HS_LEFT_HIP_HOLSTER,
								 QVR_HS_RIGHT_HIP_HOLSTER,	   QVR_HS_LEFT_UPPER_HOLSTER,	 QVR_HS_RIGHT_UPPER_HOLSTER};
	int				 i;

	// positions come from VR_XR_HolsterSpot, the same function the drawn
	// holsters use, so the place a hand has to reach and the place the holster
	// appears cannot drift apart
	(void)player_origin;

	for (i = 0; i < 6; i++)
	{
		vec3_t		spot, d;
		const float thresh = (i < 2) ? vr_shoulder_holster_thresh.value : ((i < 4) ? vr_hip_holster_thresh.value : vr_upper_holster_thresh.value);

		if (!VR_XR_HolsterSpot (order[i], spot))
			continue;

		VectorSubtract (hand_world, spot, d);
		if (VectorLength (d) < thresh)
			return order[i];
	}

	return QVR_HS_NONE;
}

/*
===============
VR_XR_HolsterSpot

The world position of one holster hotspot, for the debug visualisations to
draw. Returns false when the index is not a holster or there is no player to
hang it off.
===============
*/
qboolean VR_XR_HolsterSpot (int hotspot, vec3_t out)
{
	vec3_t player_origin, adj;
	float  mult;

	if (!VR_XR_SessionRunning ())
		return false;
	if (cl.viewentity <= 0 || cl.viewentity >= cl.max_edicts || !cl.entities)
		return false;

	VectorCopy (cl.entities[cl.viewentity].origin, player_origin);

	switch (hotspot)
	{
	case QVR_HS_LEFT_SHOULDER_HOLSTER:
	case QVR_HS_RIGHT_SHOULDER_HOLSTER:
	{
		const float sgn = (hotspot == QVR_HS_LEFT_SHOULDER_HOLSTER) ? -1.0f : 1.0f;
		XR_BodyAnchor (
			player_origin, vr_shoulder_offset_x.value + vr_shoulder_holster_offset_x.value,
			sgn * (vr_shoulder_offset_y.value + vr_shoulder_holster_offset_y.value), vr_shoulder_offset_z.value + vr_shoulder_holster_offset_z.value,
			out);
		mult = 1.5f;
		break;
	}
	case QVR_HS_LEFT_HIP_HOLSTER:
	case QVR_HS_RIGHT_HIP_HOLSTER:
	{
		const float sgn = (hotspot == QVR_HS_LEFT_HIP_HOLSTER) ? -1.0f : 1.0f;
		XR_BodyAnchor (player_origin, vr_hip_offset_x.value, sgn * vr_hip_offset_y.value, vr_hip_offset_z.value, out);
		mult = -9.5f;
		break;
	}
	case QVR_HS_LEFT_UPPER_HOLSTER:
	case QVR_HS_RIGHT_UPPER_HOLSTER:
	{
		const float sgn = (hotspot == QVR_HS_LEFT_UPPER_HOLSTER) ? -1.0f : 1.0f;
		XR_BodyAnchor (player_origin, vr_upper_holster_offset_x.value, sgn * vr_upper_holster_offset_y.value, vr_upper_holster_offset_z.value, out);
		mult = -1.5f;
		break;
	}
	default:
		return false;
	}

	// each set slides forward by its own amount as the player crouches
	XR_HolsterCrouchAdjust (mult, adj);
	VectorAdd (out, adj, out);
	return true;
}

/*
===============
VR_XR_HandDebug

Position and orientation of one hand in the world, for the debug drawing.
===============
*/
qboolean VR_XR_HandDebug (int hand, vec3_t out_pos, vec3_t out_angles)
{
	vec3_t vel;

	if (!VR_XR_SessionRunning () || hand < 0 || hand >= VR_HANDS)
		return false;
	if (!vr_xr_hand[hand].tracked)
		return false;
	if (cl.viewentity <= 0 || cl.viewentity >= cl.max_edicts || !cl.entities)
		return false;

	XR_HandToWorld (&vr_xr_hand[hand], cl.entities[cl.viewentity].origin, out_pos, out_angles, vel);
	return true;
}


/*
	VR BODY

	The player can look down and see a torso, and see their own hands with
	fingers that follow the controller's grip. These are ordinary alias
	entities positioned each frame and drawn through the view-model path, the
	same arrangement quakevr uses (client.hpp:260-286, gl_rmain.cpp:1592).

	Finger models carry six frames from open to closed; the curl computed in
	XR_UpdateFingers selects one.

================================================================================
*/

// Indices into the hand: the five fingers keep the order of xr_finger_models,
// and the palm follows them.
#define XR_HAND_PART_BASE 5

/*
XR_AliasVertexWorldPos

Where a given vertex of an entity's model ends up in the world.

quakevr uses this to hang the hand off the weapon it is holding rather than off
the controller, so the hand sits on the grip wherever the artist put it
(VR_GetScaledAndAngledAliasVertexPosition, vr.cpp:2174-2199). The matrix is
theirs, in their order: entity rotation, then the horizontal flip, then the
extra offsets, then the model's own scale_origin and scale.

Returns false when the model has no CPU-side vertices to read, which is
everything that is not a plain MDL.
===============
*/
/*
===============
XR_FindGripPointIndex

Which vertex of a weapon is its grip.

There is no answer to lift from quakevr here. Its HandAnchorVertex defaults to
0 for every weapon and no InitWeaponCVars call overrides it, but vertex 0 is
not a grip in either engine -- quakevr indexes the post-dedup VBO order, where
0 is just the first vertex of the first triangle, and this indexes raw MDL
order. quakevr's real values were dialled per weapon through its menu at
runtime, so they live in a config rather than in the source.

Geometry answers it instead. Quake viewmodels are authored pointing along +X,
so the grip is at the back of the model: take the rearmost vertices, and among
those the lowest, which is the bottom of the stock or the end of a haft. Held
weapons all share that shape, whether shotgun, axe or nailgun.

Cached per model, since it depends only on the mesh.
===============
*/
static int XR_FindGripPointIndex (aliashdr_t *hdr)
{
	float min_x, max_x, cutoff, min_z, max_z, z_cutoff, sum[3], best_d;
	int	  i, count, best;

	if (hdr->gripvalid)
		return hdr->gripvert;

	min_x = max_x = (float)hdr->anchorverts[0].v[0];
	for (i = 1; i < hdr->numverts; i++)
	{
		const float x = (float)hdr->anchorverts[i].v[0];
		if (x < min_x)
			min_x = x;
		if (x > max_x)
			max_x = x;
	}

	// the rearmost fifth of the model's length counts as "the back"
	cutoff = min_x + (max_x - min_x) * 0.2f;

	// within that, the lower half, so the barrel and receiver do not drag the
	// answer up out of the grip
	min_z = 255.0f;
	max_z = 0.0f;
	for (i = 0; i < hdr->numverts; i++)
	{
		const float z = (float)hdr->anchorverts[i].v[2];
		if ((float)hdr->anchorverts[i].v[0] > cutoff)
			continue;
		if (z < min_z)
			min_z = z;
		if (z > max_z)
			max_z = z;
	}
	z_cutoff = min_z + (max_z - min_z) * 0.5f;

	// Average the region rather than taking an extreme. The lowest vertex is
	// on the outer surface by definition -- the bottom edge of the stock -- and
	// a hand anchored there hangs off the gun instead of closing around it.
	sum[0] = sum[1] = sum[2] = 0.0f;
	count = 0;
	for (i = 0; i < hdr->numverts; i++)
	{
		if ((float)hdr->anchorverts[i].v[0] > cutoff)
			continue;
		if ((float)hdr->anchorverts[i].v[2] > z_cutoff)
			continue;
		sum[0] += (float)hdr->anchorverts[i].v[0];
		sum[1] += (float)hdr->anchorverts[i].v[1];
		sum[2] += (float)hdr->anchorverts[i].v[2];
		count++;
	}

	// Then keep the real vertex nearest that average, rather than the average
	// itself. A vertex index can be looked up again in whatever pose the model
	// is drawn in this frame, so the grip follows the firing animation; a bare
	// point could only ever describe the rest pose.
	best = 0;
	best_d = 1e30f;
	if (count > 0)
	{
		const float cx = sum[0] / (float)count;
		const float cy = sum[1] / (float)count;
		const float cz = sum[2] / (float)count;

		for (i = 0; i < hdr->numverts; i++)
		{
			float dx, dy, dz, d;
			if ((float)hdr->anchorverts[i].v[0] > cutoff)
				continue;
			if ((float)hdr->anchorverts[i].v[2] > z_cutoff)
				continue;
			dx = (float)hdr->anchorverts[i].v[0] - cx;
			dy = (float)hdr->anchorverts[i].v[1] - cy;
			dz = (float)hdr->anchorverts[i].v[2] - cz;
			d = dx * dx + dy * dy + dz * dz;
			if (d < best_d)
			{
				best_d = d;
				best = i;
			}
		}
	}

	hdr->gripvert = best;
	hdr->gripvalid = true;
	return best;
}

static qboolean XR_AliasVertexWorldPos (entity_t *e, int vertex_index, const vec3_t extra_offsets, vec3_t out)
{
	aliashdr_t *hdr;
	vec3_t		fwd, right, up, local, v, raw;
	float		pitch_flipped[3];

	int			i;

	if (!e || !e->model || e->model->type != mod_alias)
		return false;

	hdr = (aliashdr_t *)Mod_Extradata (e->model);
	if (!hdr || !hdr->anchorverts || hdr->numverts <= 0)
		return false;

	if (vertex_index < 0)
		vertex_index = XR_FindGripPointIndex (hdr);

	vertex_index = CLAMP (0, vertex_index, hdr->numverts - 1);

	// Read that vertex from the pose the weapon is actually drawn in this
	// frame, not from the rest pose. A firing animation moves the grip -- that
	// is what a kick is -- and reading a fixed pose leaves the hand behind
	// while the weapon recoils out of it.
	{
		lerpdata_t ld;
		int		   pose;
		R_SetupAliasFrame (e, hdr, &ld);
		pose = CLAMP (0, ld.pose1, hdr->numposes - 1);

		raw[0] = (float)hdr->anchorverts[(size_t)pose * hdr->numverts + vertex_index].v[0];
		raw[1] = (float)hdr->anchorverts[(size_t)pose * hdr->numverts + vertex_index].v[1];
		raw[2] = (float)hdr->anchorverts[(size_t)pose * hdr->numverts + vertex_index].v[2];
	}
	// the vertex in model space, then the model's own transform
	for (i = 0; i < 3; i++)
		v[i] = raw[i] * hdr->scale[i] + hdr->scale_origin[i] + extra_offsets[i];

	// mirrored models put their grip on the other side
	if (e->horizFlip)
		v[1] = -v[1];

	// R_RotateForEntity negates pitch on the way in, so undo that here to get
	// the basis the model is actually drawn in
	pitch_flipped[PITCH] = -e->angles[PITCH];
	pitch_flipped[YAW] = e->angles[YAW];
	pitch_flipped[ROLL] = e->angles[ROLL];
	AngleVectors (pitch_flipped, fwd, right, up);

	// Quake's local axes are X forward, Y left, Z up; AngleVectors gives right
	local[0] = v[0] * fwd[0] - v[1] * right[0] + v[2] * up[0];
	local[1] = v[0] * fwd[1] - v[1] * right[1] + v[2] * up[1];
	local[2] = v[0] * fwd[2] - v[1] * right[2] + v[2] * up[2];

	VectorAdd (e->origin, local, out);
	return true;
}


/*
XR_HandPartOffset

quakevr's fingerIdxToOffset (view.cpp:1370-1408). The terms accumulate rather
than override: every part gets the whole-hand offset, the off hand gets a
further term on top of that so the two hands can be tuned apart, and only the
fingers get the fingers-as-a-group term before their own individual one. The
palm takes the base term instead and never sees the group term.
===============
*/
static void XR_HandPartOffset (int part, int hand, vec3_t out)
{
	out[0] = vr_fingers_and_base_x.value;
	out[1] = vr_fingers_and_base_y.value;
	out[2] = vr_fingers_and_base_z.value;

	if (hand == VR_XR_OffHand ())
	{
		out[0] += vr_fingers_and_base_offhand_x.value;
		out[1] += vr_fingers_and_base_offhand_y.value;
		out[2] += vr_fingers_and_base_offhand_z.value;
	}

	if (part == XR_HAND_PART_BASE)
	{
		out[0] += vr_finger_base_x.value;
		out[1] += vr_finger_base_y.value;
		out[2] += vr_finger_base_z.value;
		return;
	}

	out[0] += vr_fingers_x.value;
	out[1] += vr_fingers_y.value;
	out[2] += vr_fingers_z.value;

	switch (part)
	{
	case VR_FINGER_THUMB:
		out[0] += vr_finger_thumb_x.value;
		out[1] += vr_finger_thumb_y.value;
		out[2] += vr_finger_thumb_z.value;
		break;
	case VR_FINGER_INDEX:
		out[0] += vr_finger_index_x.value;
		out[1] += vr_finger_index_y.value;
		out[2] += vr_finger_index_z.value;
		break;
	case VR_FINGER_MIDDLE:
		out[0] += vr_finger_middle_x.value;
		out[1] += vr_finger_middle_y.value;
		out[2] += vr_finger_middle_z.value;
		break;
	case VR_FINGER_RING:
		out[0] += vr_finger_ring_x.value;
		out[1] += vr_finger_ring_y.value;
		out[2] += vr_finger_ring_z.value;
		break;
	default:
		out[0] += vr_finger_pinky_x.value;
		out[1] += vr_finger_pinky_y.value;
		out[2] += vr_finger_pinky_z.value;
		break;
	}
}

static const char *xr_finger_models[5] = {
	"progs/finger_thumb.mdl", "progs/finger_index.mdl", "progs/finger_middle.mdl", "progs/finger_ring.mdl", "progs/finger_pinky.mdl"};

/*
XR_SetupHolsterSlots

The visible holsters. quakevr draws progs/legholster.mdl at four of the six
hotspots -- both hips and both uppers, not the shoulders -- each with its own
angles (view.cpp V_RenderView_HolsterModels, 1681-1704):

  left hip     pitch   0, yaw bodyYaw - 10, mirrored
  right hip    pitch   0, yaw bodyYaw + 10
  left upper   pitch -30, yaw bodyYaw - 10, mirrored
  right upper  pitch -30, yaw bodyYaw + 10

The ten-degree splay and the thirty-degree tilt on the uppers are what make
these read as worn on a body rather than floating beside one, and mirroring the
left pair is the same horizFlip the off hand uses.

Positions come from VR_XR_HolsterSpot, so a holster is drawn exactly where the
hand has to reach to trigger it.
===============
*/
static void XR_SetupHolsterSlots (void)
{
	static const int	  hotspots[4] = {QVR_HS_LEFT_HIP_HOLSTER, QVR_HS_RIGHT_HIP_HOLSTER, QVR_HS_LEFT_UPPER_HOLSTER, QVR_HS_RIGHT_UPPER_HOLSTER};
	static const float	  pitches[4] = {0.0f, 0.0f, -30.0f, -30.0f};
	static const float	  yaw_offsets[4] = {-10.0f, 10.0f, -10.0f, 10.0f};
	static const qboolean flips[4] = {true, false, true, false};
	int					  i;

	for (i = 0; i < 4; i++)
	{
		entity_t *e = &cl.vrlegholster[i];
		vec3_t	  spot;

		if (!vr_leg_holster_model_enabled.value || !VR_XR_HolsterSpot (hotspots[i], spot))
		{
			e->model = NULL;
			continue;
		}

		VectorCopy (spot, e->origin);
		e->angles[PITCH] = pitches[i];
		e->angles[YAW] = cl.viewangles[YAW] + yaw_offsets[i];
		e->angles[ROLL] = 0.0f;

		e->model = Mod_ForName ("progs/legholster.mdl", false);
		e->frame = 0;
		e->colormap = vid.colormap;
		e->alpha = ENTALPHA_DEFAULT;
		e->netstate.scale = ENTSCALE_DEFAULT;
		e->horizFlip = flips[i];
	}
}

/*
XR_SetupHandEntity

Places the palm and its five fingers at a hand. All six share the hand's
transform; only the frame differs per finger.
===============
*/
static void XR_SetupHandEntity (int hand, const vec3_t player_origin)
{
	const vr_hand_t *h = &vr_xr_hand[hand];
	entity_t		*palm = &cl.vrhand[hand];
	vec3_t			 world, angles, vel;
	vec3_t			 fwd, right, up, ofs;
	int				 i;

	if (!h->tracked)
	{
		// no pose this frame: draw nothing rather than leave it where it was
		palm->model = NULL;
		for (i = 0; i < 5; i++)
			cl.vrfinger[hand][i].model = NULL;
		return;
	}

	XR_HandToWorld (h, player_origin, world, angles, vel);

	// Sit the hand on the weapon's grip rather than at the controller. The
	// weapon is already placed at the hand, so this walks the last step the
	// other way: find where the grip vertex ended up and move the hand there.
	// Both hands, each to whichever weapon it is holding.
	if (vr_hand_grips_weapon.value)
	{
		entity_t *wpn = (hand == VR_XR_MainHand ()) ? &cl.viewent : &cl.offhand_viewent;
		vec3_t	  grip, none = {0.0f, 0.0f, 0.0f};

		if (wpn->model && XR_AliasVertexWorldPos (wpn, (int)vr_grip_vertex.value, none, grip))
			VectorCopy (grip, world);
	}


	// quakevr builds the hand out of six separate models, each with its own
	// offset, and applies that offset inside the hand's own rotated frame --
	// fingerIdxToOffset supplies it (view.cpp:1370-1408) and the anchor matrix
	// translates by it after the rotations, in
	// VR_GetScaledAndAngledAliasVertexPosition. Rotating it here is the same
	// thing: a local translate inside the entity matrix.
	//
	// Quake's local axes are X forward, Y left, Z up, and AngleVectors hands
	// back right rather than left, hence the negated Y term.
	AngleVectors (angles, fwd, right, up);

	XR_HandPartOffset (XR_HAND_PART_BASE, hand, ofs);
	palm->origin[0] = world[0] + ofs[0] * fwd[0] - ofs[1] * right[0] + ofs[2] * up[0];
	palm->origin[1] = world[1] + ofs[0] * fwd[1] - ofs[1] * right[1] + ofs[2] * up[1];
	palm->origin[2] = world[2] + ofs[0] * fwd[2] - ofs[1] * right[2] + ofs[2] * up[2];

	VectorCopy (angles, palm->angles);
	palm->angles[PITCH] = -palm->angles[PITCH]; // alias models draw pitch inverted
	// hand_base.mdl, not hand.mdl. hand.mdl is quakevr's empty-hand *weapon*
	// placeholder, which it registers with scale 0 (vr.cpp:1069) precisely so
	// it never draws; it is a whole closed fist, far too big for a palm the
	// five finger models attach to. The visible hand is hand_entities.base,
	// and view.cpp:1368 names that model hand_base.mdl.
	palm->model = Mod_ForName ("progs/hand_base.mdl", false);
	palm->frame = 0;
	palm->colormap = vid.colormap;
	palm->alpha = ENTALPHA_DEFAULT;
	// Without this the model is scaled to nothing: these entities live in cl,
	// which is memset on connect, and ENTSCALE_DECODE(0) is 0.
	palm->netstate.scale = ENTSCALE_DEFAULT;
	palm->alphatestonly = true;
	// One hand model serves both hands, mirrored on Y for the off hand -- the
	// same thing quakevr does with horizFlip (r_alias.cpp:1208).
	palm->horizFlip = (hand == VR_XR_OffHand ());

	for (i = 0; i < 5; i++)
	{
		entity_t *f = &cl.vrfinger[hand][i];

		XR_HandPartOffset (i, hand, ofs);
		f->origin[0] = world[0] + ofs[0] * fwd[0] - ofs[1] * right[0] + ofs[2] * up[0];
		f->origin[1] = world[1] + ofs[0] * fwd[1] - ofs[1] * right[1] + ofs[2] * up[1];
		f->origin[2] = world[2] + ofs[0] * fwd[2] - ofs[1] * right[2] + ofs[2] * up[2];

		VectorCopy (palm->angles, f->angles);
		f->model = Mod_ForName (xr_finger_models[i], false);
		// curl 0..5 selects the frame; clamp because a model may ship fewer
		f->frame = (int)CLAMP (0.0f, h->finger[i], 5.0f);
		f->colormap = vid.colormap;
		f->alpha = ENTALPHA_DEFAULT;
		f->netstate.scale = ENTSCALE_DEFAULT;
		f->alphatestonly = true;
		f->horizFlip = palm->horizFlip;
	}
}

/*
XR_SetupOffHandWeapon

Dual wielding. quakevr ships the off-hand weapon's model and frame to the
client through extra stat bytes on its own protocol (SU_VR_WEAPON2 and friends,
protocol.hpp:262-264). Rather than extend the protocol here, the values are
read straight off the player edict, which works because the QC that sets them
is running in the same process on a listen server.
(quakevr V_SetupOffHandWpnViewEnt, view.cpp:1167-1220)
===============
*/
static void XR_SetupOffHandWeapon (const vec3_t player_origin)
{
	const struct pr_extfields_s *f;
	entity_t					*view = &cl.offhand_viewent;
	const vr_hand_t				*h;
	edict_t						*player;
	eval_t						*model_field;
	const char					*model_name;
	vec3_t						 world, angles, vel;
	qcvm_t						*old_vm = NULL;
	qboolean					 switched = false;

	view->model = NULL;

	if (!sv.active || cls.signon != SIGNONS)
		return;

	if (qcvm != &sv.qcvm)
	{
		old_vm = qcvm;
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);
		switched = true;
	}

	f = &qcvm->extfields;
	player = (cl.viewentity > 0 && cl.viewentity < qcvm->num_edicts) ? EDICT_NUM (cl.viewentity) : NULL;

	if (player && !player->free && f->weaponmodel2 >= 0)
	{
		model_field = GetEdictFieldValue (player, f->weaponmodel2);
		if (model_field && model_field->string)
		{
			model_name = PR_GetString (model_field->string);
			if (model_name && *model_name)
			{
				view->model = Mod_ForName (model_name, false);
				if (f->weaponframe2 >= 0)
				{
					eval_t *fr = GetEdictFieldValue (player, f->weaponframe2);
					view->frame = fr ? (int)fr->_float : 0;
				}
			}
		}
	}

	if (switched)
	{
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (old_vm);
	}

	if (!view->model)
		return;

	h = &vr_xr_hand[VR_XR_OffHand ()];
	if (!h->tracked)
	{
		view->model = NULL;
		return;
	}

	XR_HandToWorld (h, player_origin, world, angles, vel);
	VectorCopy (world, view->origin);
	VectorCopy (angles, view->angles);
	view->angles[PITCH] = -view->angles[PITCH];
	view->colormap = vid.colormap;
	view->alpha = ENTALPHA_DEFAULT;
	view->netstate.scale = ENTSCALE_DEFAULT;
}

/*
VR_XR_SetupBodyEntities
===============
*/
void VR_XR_SetupBodyEntities (void)
{
	entity_t *player;
	vec3_t	  fwd, right, up, yaw_only;
	float	  head_height;
	int		  hand;

	if (!VR_XR_SessionRunning () || cls.state != ca_connected || cls.signon != SIGNONS)
		return;
	if (cl.viewentity <= 0 || cl.viewentity >= cl.max_edicts || !cl.entities)
		return;

	player = &cl.entities[cl.viewentity];

	for (hand = 0; hand < VR_HANDS; hand++)
		XR_SetupHandEntity (hand, player->origin);

	XR_SetupOffHandWeapon (player->origin);
	XR_SetupHolsterSlots ();

	// --- torso (quakevr V_SetupVRTorsoViewEnt, view.cpp:1222-1249) ---
	if (!vr_vrtorso_enabled.value)
	{
		cl.vrtorso.model = NULL;
		return;
	}

	yaw_only[PITCH] = 0.0f;
	yaw_only[YAW] = cl.viewangles[YAW];
	yaw_only[ROLL] = 0.0f;
	AngleVectors (yaw_only, fwd, right, up);

	const float torso_crouch = CLAMP (0.0f, XR_CrouchRatio (), 0.8f);

	// quakevr tilts the torso forward and slides it back as the player folds
	// over (view.cpp:1233, 1244), so it follows the body through a crouch
	// instead of staying bolt upright.
	cl.vrtorso.angles[PITCH] = vr_vrtorso_pitch.value - (torso_crouch * 35.0f);
	cl.vrtorso.angles[YAW] = cl.viewangles[YAW] + vr_vrtorso_yaw.value;
	cl.vrtorso.angles[ROLL] = vr_vrtorso_roll.value;

	cl.vrtorso.model = Mod_ForName ("progs/vrtorso.mdl", false);
	cl.vrtorso.frame = 0;
	cl.vrtorso.colormap = vid.colormap;
	cl.vrtorso.alpha = ENTALPHA_DEFAULT;
	cl.vrtorso.netstate.scale = ENTSCALE_DEFAULT;
	cl.vrtorso.alphatestonly = true;

	VectorCopy (player->origin, cl.vrtorso.origin);
	VectorMA (cl.vrtorso.origin, vr_vrtorso_x_offset.value, fwd, cl.vrtorso.origin);
	VectorMA (cl.vrtorso.origin, -(torso_crouch * 14.0f), fwd, cl.vrtorso.origin);
	VectorMA (cl.vrtorso.origin, vr_vrtorso_y_offset.value, right, cl.vrtorso.origin);

	// quakevr's own arithmetic, which works now that the world scale is its own
	// (view.cpp:1245-1246). VR_GetHeadOrigin is in metres, so the stored head
	// height is converted back and vr_floor_offset removed.
	//
	// This failed earlier only because the world scale was the code default of
	// 1.0, giving 26.25 units per metre against the head_z_mult of 33 that
	// quakevr's shipped config assumes. At its shipped world scale of 1.25 --
	// 32.8 units per metre, near Quake's native 32 -- the multiplier and the
	// -45 offset land the torso where they were tuned to.
	head_height = xr_head_pos_valid ? ((xr_last_head_pos[2] - vr_floor_offset.value) / VR_METERS_TO_UNITS) : 0.0f;
	cl.vrtorso.origin[2] += head_height * vr_vrtorso_head_z_mult.value;
	cl.vrtorso.origin[2] += vr_vrtorso_z_offset.value;
}

/*
	VR HUD

	A status bar painted flat across both eyes has no depth, so it fights the
	stereo image and is uncomfortable to read. quakevr instead hangs the 2D
	canvas on a panel floating in front of the player and renders the ordinary
	HUD onto it (vr.cpp:3588-3700).

	quakevr composes that with the fixed-function matrix stack. Vulkan has no
	such stack -- vkQuake pushes the 2D ortho matrix as a push constant -- so
	the same transform is built directly and substituted for the ortho matrix.

================================================================================
*/

cvar_t vr_menu_scale = {"vr_menu_scale", "0.15", CVAR_ARCHIVE};
cvar_t vr_menu_distance = {"vr_menu_distance", "80", CVAR_ARCHIVE};
cvar_t vr_hud_enabled = {"vr_hud_enabled", "1", CVAR_ARCHIVE};

static vec3_t xr_hud_last_pos;
static qboolean xr_hud_pos_valid = false;

/*
VR_XR_HudMatrix

Returns false when the flat ortho matrix should be used instead.

Canvas coordinates are 320x200 with Y running down. The panel is placed a fixed
distance along the view direction, turned to face the player, and centred; the
position is smoothed so the HUD does not jitter with every head movement.
(quakevr vr.cpp:3646-3680)
===============
*/
qboolean VR_XR_HudMatrix (float out[16], float canvas_w, float canvas_h)
{
	vec3_t	fwd, right, up, angles, target;
	float	m[16], tmp[16];
	float	scale;

	if (!vr_hud_enabled.value || !VR_XR_SessionRunning ())
		return false;
	if (cls.state != ca_connected || !cl.worldmodel)
		return false;

	scale = vr_menu_scale.value;

	// hang it off the view direction, upright
	VectorCopy (r_refdef.viewangles, angles);
	AngleVectors (angles, fwd, right, up);
	VectorMA (r_refdef.vieworg, vr_menu_distance.value, fwd, target);

	// Smooth toward the target rather than snapping. quakevr mixes at 0.9
	// (vr.cpp:3663); without it the panel shakes with every small head motion.
	if (!xr_hud_pos_valid)
	{
		VectorCopy (target, xr_hud_last_pos);
		xr_hud_pos_valid = true;
	}
	else
	{
		xr_hud_last_pos[0] += (target[0] - xr_hud_last_pos[0]) * 0.9f;
		xr_hud_last_pos[1] += (target[1] - xr_hud_last_pos[1]) * 0.9f;
		xr_hud_last_pos[2] += (target[2] - xr_hud_last_pos[2]) * 0.9f;
	}
	VectorCopy (xr_hud_last_pos, target);

	// quakevr's status-bar placement offsets, applied in the panel's own frame
	// so they read as "up", "right" and "closer" from where the player is
	// looking rather than as world directions (vr_cvars.cpp:71-77).
	VectorMA (target, vr_sbar_offset_x.value, fwd, target);
	VectorMA (target, vr_sbar_offset_y.value, right, target);
	VectorMA (target, vr_sbar_offset_z.value, up, target);

	angles[PITCH] += vr_sbar_offset_pitch.value;
	angles[YAW] += vr_sbar_offset_yaw.value;
	angles[ROLL] += vr_sbar_offset_roll.value;


	// quakevr's order, expressed as matrices instead of GL calls:
	//   T(target) Rz(yaw-90) Rx(-(pitch+90)) T(-w*scale/2, -h*scale/2, 0) S(scale)
	IdentityMatrix (m);

	TranslationMatrix (tmp, target[0], target[1], target[2]);
	MatrixMultiply (m, tmp);

	RotationMatrix (tmp, DEG2RAD (angles[YAW] - 90.0f), 0.0f, 0.0f, 1.0f);
	MatrixMultiply (m, tmp);

	RotationMatrix (tmp, DEG2RAD (-(angles[PITCH] + 90.0f)), 1.0f, 0.0f, 0.0f);
	MatrixMultiply (m, tmp);

	TranslationMatrix (tmp, -(canvas_w * scale * 0.5f), -(canvas_h * scale * 0.5f), 0.0f);
	MatrixMultiply (m, tmp);

	ScaleMatrix (tmp, scale, scale, scale);
	MatrixMultiply (m, tmp);

	// finally through the eye's view-projection so it lands in the right place
	// for each eye and gets real stereo depth
	memcpy (out, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
	MatrixMultiply (out, m);
	return true;
}

/*
VR_XR_HandTouch

Reaching for something is how you pick it up in VR, so items cannot rely on the
player's body walking through them. quakevr adds a second touch path driven by
the hand positions, and its QC hangs holsters, force-grab and item pickup off
the .handtouch field it invokes. Ported from quakevr world.cpp:448-503.
===============
*/

// quakevr allows body-based interaction as an option (vr_cvars.cpp)
cvar_t vr_body_interactions = {"vr_body_interactions", "0", CVAR_ARCHIVE};

// Throw tuning, defaults from quakevr (vr_cvars.cpp:147-194)
cvar_t vr_throw_avg_frames = {"vr_throw_avg_frames", "15", CVAR_ARCHIVE};
cvar_t vr_throw_angvel_avg_frames = {"vr_throw_angvel_avg_frames", "5", CVAR_ARCHIVE};
cvar_t vr_weapon_throw_velocity_mult = {"vr_weapon_throw_velocity_mult", "1.0", CVAR_ARCHIVE};
cvar_t vr_weapon_throw_damage_mult = {"vr_weapon_throw_damage_mult", "1.0", CVAR_ARCHIVE};
cvar_t vr_weapon_throw_mode = {"vr_weapon_throw_mode", "0", CVAR_ARCHIVE};

// hands are given some size so they do not have to be pixel-perfect
#define VR_HAND_TOUCH_SIZE 2.5f
// and flagged entities get more, so awkward pickups stay reachable
// (quakevr VR_GetEasyHandTouchBonus, vr.cpp:624-627)
#define VR_EASY_HAND_TOUCH_BONUS 4.5f

static qboolean XR_BoxIntersect (const vec3_t amin, const vec3_t amax, const vec3_t bmin, const vec3_t bmax)
{
	if (amin[0] > bmax[0] || amin[1] > bmax[1] || amin[2] > bmax[2])
		return false;
	if (amax[0] < bmin[0] || amax[1] < bmin[1] || amax[2] < bmin[2])
		return false;
	return true;
}

void VR_XR_HandTouch (edict_t *ent, edict_t *target)
{
	const struct pr_extfields_s *f;
	eval_t						*handtouch;
	eval_t						*hp;
	eval_t						*ohp;
	vec3_t						 hmin, hmax, tmin, tmax;
	float						 bonus;
	qboolean					 off_hit = false, main_hit = false;
	int							 old_self, old_other;

	if (!ent || !target || !qcvm)
		return;

	f = &qcvm->extfields;
	if (f->handtouch < 0)
		return; // this progs.dat has no .handtouch; nothing to call

	// quakevr: canBeHandTouched -- must have the callback and be solid
	// (util.hpp:298-302)
	handtouch = GetEdictFieldValue (target, f->handtouch);
	if (!handtouch || !handtouch->function || target->v.solid == SOLID_NOT)
		return;

	hp = (f->handpos >= 0) ? GetEdictFieldValue (ent, f->handpos) : NULL;
	ohp = (f->offhandpos >= 0) ? GetEdictFieldValue (ent, f->offhandpos) : NULL;
	if (!hp && !ohp)
		return;

	// the flag is on the thing being grabbed, not the grabber
	bonus = ((int)target->v.flags & FL_EASYHANDTOUCH) ? VR_EASY_HAND_TOUCH_BONUS : 0.0f;

	tmin[0] = target->v.absmin[0] - bonus;
	tmin[1] = target->v.absmin[1] - bonus;
	tmin[2] = target->v.absmin[2] - bonus;
	tmax[0] = target->v.absmax[0] + bonus;
	tmax[1] = target->v.absmax[1] + bonus;
	tmax[2] = target->v.absmax[2] + bonus;

	if (ohp)
	{
		hmin[0]=ohp->vector[0]-VR_HAND_TOUCH_SIZE; hmin[1]=ohp->vector[1]-VR_HAND_TOUCH_SIZE; hmin[2]=ohp->vector[2]-VR_HAND_TOUCH_SIZE;
		hmax[0]=ohp->vector[0]+VR_HAND_TOUCH_SIZE; hmax[1]=ohp->vector[1]+VR_HAND_TOUCH_SIZE; hmax[2]=ohp->vector[2]+VR_HAND_TOUCH_SIZE;
		off_hit = XR_BoxIntersect (hmin, hmax, tmin, tmax);
	}
	if (hp)
	{
		hmin[0]=hp->vector[0]-VR_HAND_TOUCH_SIZE; hmin[1]=hp->vector[1]-VR_HAND_TOUCH_SIZE; hmin[2]=hp->vector[2]-VR_HAND_TOUCH_SIZE;
		hmax[0]=hp->vector[0]+VR_HAND_TOUCH_SIZE; hmax[1]=hp->vector[1]+VR_HAND_TOUCH_SIZE; hmax[2]=hp->vector[2]+VR_HAND_TOUCH_SIZE;
		main_hit = XR_BoxIntersect (hmin, hmax, tmin, tmax);
	}

	if (!off_hit && !main_hit)
		return;

	// tell the QC which hand did it (quakevr VR_SetHandtouchParams, vr.cpp:678-683)
	{
		const int hand = off_hit ? 0 : 1;
		XR_SetFloat (ent, f->touchinghand, (float)hand);
		XR_SetFloat (target, f->handtouch_hand, (float)hand);
		if (f->handtouch_ent >= 0)
		{
			eval_t *he = GetEdictFieldValue (target, f->handtouch_ent);
			if (he)
				he->edict = EDICT_TO_PROG (ent);
		}
	}

	old_self = pr_global_struct->self;
	old_other = pr_global_struct->other;

	pr_global_struct->self = EDICT_TO_PROG (target);
	pr_global_struct->other = EDICT_TO_PROG (ent);
	pr_global_struct->time = qcvm->time;
	PR_ExecuteProgram (handtouch->function);

	pr_global_struct->self = old_self;
	pr_global_struct->other = old_other;
}

/*
VR_XR_Haptic
===============
*/
void VR_XR_Haptic (int hand, float duration, float frequency, float amplitude)
{
	// quakevr checks this before every pulse (vr.cpp:3901)
	if (vr_disablehaptics.value == 1)
		return;

	XrHapticActionInfo info;
	XrHapticVibration  vibration;

	if (!xr_input_ready || hand < 0 || hand >= VR_HANDS || !xr_act_haptic)
		return;

	memset (&vibration, 0, sizeof (vibration));
	vibration.type = XR_TYPE_HAPTIC_VIBRATION;
	vibration.duration = (XrDuration)(duration * 1000000000.0f); // seconds -> nanoseconds
	vibration.frequency = frequency;
	vibration.amplitude = CLAMP (0.0f, amplitude, 1.0f);

	memset (&info, 0, sizeof (info));
	info.type = XR_TYPE_HAPTIC_ACTION_INFO;
	info.action = xr_act_haptic;
	info.subactionPath = xr_hand_path[hand];

	xrApplyHapticFeedback (xr_session, &info, (const XrHapticBaseHeader *)&vibration);
}

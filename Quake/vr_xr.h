/*
Copyright (C) 2026 Quake VR contributors

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
// vr_xr.h -- OpenXR runtime integration

#ifndef _VR_XR_H
#define _VR_XR_H

#include <vulkan/vulkan.h>

// true once an OpenXR instance + HMD system have been acquired.
// stays false when -vr is absent, no runtime is installed, or no headset is connected.
extern qboolean vr_xr_active;

// per-eye render target size recommended by the runtime (valid when vr_xr_active)
extern uint32_t vr_xr_eye_width;
extern uint32_t vr_xr_eye_height;

// called from Host_Init before VID_Init, so the Vulkan instance/device can be
// created against the runtime's requirements. never fatal: on any failure it
// reports to the console, leaves vr_xr_active false, and the engine runs flat.
void VR_XR_Init (void);
void VR_XR_Shutdown (void);

// --- Vulkan creation, routed through the runtime ---
// OpenXR must create the instance/device itself (XR_KHR_vulkan_enable2) so it can
// add the extensions the compositor needs, and it dictates which GPU to use --
// on a multi-GPU box that is whichever one the headset is actually attached to.
// All three return false when VR is inactive, in which case the caller does its
// normal thing. On a true return, *vk_err holds the underlying VkResult.
qboolean VR_XR_CreateVulkanInstance (PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo *create_info, VkInstance *out_instance, VkResult *vk_err);
qboolean VR_XR_GetVulkanPhysicalDevice (PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice *out_device);
qboolean VR_XR_CreateVulkanDevice (
	PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info, VkDevice *out_device, VkResult *vk_err);

// --- session ---
// called once from GL_InitDevice, after the graphics queue exists.
void VR_XR_CreateSession (VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family_index, uint32_t queue_index);

// pumps the OpenXR event queue and drives the session state machine.
// safe to call every host frame; cheap no-op when VR is inactive.
void VR_XR_PumpEvents (void);

// true once the runtime is actually presenting us to the user, i.e. the session
// reached SYNCHRONIZED/VISIBLE/FOCUSED. Until then we must not submit frames.
qboolean VR_XR_SessionRunning (void);

// wait/begin/end around a frame. VR_XR_BeginFrame reports whether the runtime
// wants this frame rendered at all -- it can ask us to idle while the headset
// is off the head, and burning GPU on those frames is wasted.
qboolean VR_XR_BeginFrame (void);
void	 VR_XR_EndFrame (void);

// per-eye swapchain image count and per-eye images, valid after CreateSession
#define VR_XR_EYES 2

// Records a blit of the finished frame into one eye's swapchain image. Records
// Vulkan commands only -- no OpenXR calls -- so it is safe from a task worker.
// which_eye must be passed explicitly rather than read from vr_xr_current_eye,
// because this runs asynchronously and the global will have moved on.
qboolean VR_XR_BlitToEye (VkCommandBuffer cb, VkImage src, uint32_t src_width, uint32_t src_height, int which_eye);

// Hands both eye images back to the compositor. Main thread only.
void VR_XR_ReleaseEyes (void);

// --- stereo ---
// Which eye is being rendered right now, or -1 outside the per-eye passes.
extern int vr_xr_current_eye;

// Both lifted from quakevr, which had them tuned against real play.
// vr_world_scale is a multiplier where 1.0 is life size (a Quake unit is 1.5in,
// so ~26.25 units/metre). vr_floor_offset reconciles the runtime's floor-
// relative head height with Quake's centre-of-bounding-box player origin.
extern cvar_t vr_world_scale;
extern cvar_t vr_floor_offset;

// Projection for the eye currently being rendered. Returns false when not in a
// per-eye pass, so the caller keeps its normal symmetric frustum.
// HMD lenses are not centred, so the runtime hands us an asymmetric frustum --
// forcing a symmetric fov onto it is exactly what makes the image feel wrong.
qboolean VR_XR_EyeProjectionMatrix (float matrix[16], float farclip);

// Head pose for the eye currently being rendered, converted into Quake's
// conventions: angles as pitch/yaw/roll in degrees, offset in Quake units.
// Returns false when not in a per-eye pass.
qboolean VR_XR_EyePose (float out_angles[3], float out_offset[3]);

// --- controllers ---
#define VR_HAND_LEFT  0
#define VR_HAND_RIGHT 1
#define VR_HANDS	  2

// Per-hand state, already converted into Quake conventions: position in Quake
// units relative to the play space origin, angles as pitch/yaw/roll degrees.
// Mirrors the inputs quakevr drives its gameplay from (see its VR_DoInput).
typedef struct
{
	qboolean tracked;	// runtime is reporting a valid pose this frame
	vec3_t	 pos;		// grip pose - where the hand physically is
	vec3_t	 angles;	// grip orientation
	vec3_t	 aim_pos;	// aim pose - the controller's pointing ray, used for weapons
	vec3_t	 aim_angles;

	float trigger; // 0..1, main fire
	float grip;	   // 0..1, grab
	float stick[2];

	vec3_t velocity; // Quake units/sec, from the runtime rather than differenced
	float  speed;	 // magnitude of the above

	// Rolling mean of recent velocity. Throwing uses this rather than the
	// instantaneous value: the hand decelerates sharply as it releases, so a
	// single frame under-reads the throw badly. (quakevr vr.cpp:1983-1990)
	vec3_t throw_velocity;
	vec3_t angular_velocity;

	// Per-finger curl, 0 (open) to 5 (closed) -- the frame number of the finger
	// models quakevr ships. Order matches its FingerIdx.
	// (quakevr handSkeletalToFrame, vr.cpp:3270-3299)
	float finger[5];

	qboolean btn_a;		 // A / X
	qboolean btn_b;		 // B / Y
	qboolean btn_stick;	 // thumbstick click
	qboolean btn_menu;	 // menu / system-adjacent
} vr_hand_t;

extern vr_hand_t vr_xr_hand[VR_HANDS];

// Called once from VR_XR_CreateSession; safe to fail (input just stays inert).
void VR_XR_InitInput (void);

// Pulls fresh controller state. Called once per frame from the host loop.
void VR_XR_SyncInput (void);

// Fires a haptic pulse on a hand. This is quakevr's builtin #81 equivalent and
// the one piece of VR that already had a clean QuakeC-facing API.
void VR_XR_Haptic (int hand, float duration, float frequency, float amplitude);

// --- gameplay ---
// Folds controller input into the player's movement command. Called from
// CL_BaseMove after the keyboard has had its say, so keyboard and controller
// both work and neither cancels the other out.
void VR_XR_Move (usercmd_t *cmd);

// Applies stick turning to cl.viewangles. Separate from VR_XR_Move because
// turning mutates view state rather than the command.
void VR_XR_AdjustAngles (void);

// Additional button bits to OR into cmd->buttons (bit 1 = attack, 2 = jump).
unsigned int VR_XR_Buttons (void);

extern cvar_t vr_turn_speed;
extern cvar_t vr_snap_turn;
extern cvar_t vr_deadzone;
extern cvar_t vr_roomscale;
extern cvar_t vr_roomscale_mult;
extern cvar_t vr_aim_hand;    // 0 = left, 1 = right
extern cvar_t vr_hand_aiming; // 0 = aim with the head, as before

// Direction the weapon should fire in, from the aiming hand's pointing ray.
// Returns false when hand aiming is off or the hand is not tracked, in which
// case the caller keeps using the view angles.
qboolean VR_XR_AimAngles (vec3_t out_angles);

// Places the weapon viewmodel in the aiming hand. player_origin is the player
// entity's origin, which the hand pose is relative to. Returns false when hand
// aiming is off, leaving the caller's normal head-attached gun alone.
qboolean VR_XR_WeaponPose (const vec3_t player_origin, vec3_t out_origin, vec3_t out_angles);

// Per-hand linear speed in Quake units/sec, for velocity-driven melee.
float VR_XR_HandSpeed (int hand);

// Impulse to send this frame (weapon switching from the off hand), or 0.
int VR_XR_Impulse (void);

// Places a dynamic light where the aiming hand is pointing, as a laser dot.
// Called once per frame from the host loop.
void VR_XR_UpdateLaser (void);

extern cvar_t vr_gun_offset_x;
extern cvar_t vr_gun_offset_y;
extern cvar_t vr_gun_offset_z;
extern cvar_t vr_melee_threshold;
extern cvar_t vr_haptics;
extern cvar_t vr_laser;
extern cvar_t vr_two_handed;
extern cvar_t vr_two_hand_dist;
extern cvar_t vr_height_calibration;
extern cvar_t vr_gunmodelscale;
extern cvar_t vr_gunmodely;

// Per-weapon model offset and scale, so the gun sits correctly in the hand.
// Values are quakevr's, tuned against real play (vr.cpp:1029-1064).
struct aliashdr_s;
void VR_XR_ApplyWeaponModelMod (struct aliashdr_s *hdr, const char *model_name);
void VR_XR_ModAllWeapons (void);
// weapons plus the player's body and leg holsters (quakevr VR_ModAllModels)
void VR_XR_ModAllModels (void);

// Positions the VR body: torso, both hands, and the per-finger models whose
// frame is driven by finger curl. Called once per frame from V_CalcRefdef.
void VR_XR_SetupBodyEntities (void);

extern cvar_t vr_vrtorso_enabled;
extern cvar_t vr_vrtorso_x_offset;
extern cvar_t vr_vrtorso_y_offset;
extern cvar_t vr_vrtorso_z_offset;
extern cvar_t vr_vrtorso_head_z_mult;
extern cvar_t vr_vrtorso_x_scale;
extern cvar_t vr_vrtorso_y_scale;
extern cvar_t vr_vrtorso_z_scale;
extern cvar_t vr_vrtorso_pitch;
extern cvar_t vr_vrtorso_yaw;
extern cvar_t vr_vrtorso_roll;
extern cvar_t vr_leg_holster_model_enabled;
extern cvar_t vr_leg_holster_model_scale;
extern cvar_t vr_leg_holster_model_x_offset;
extern cvar_t vr_leg_holster_model_y_offset;
extern cvar_t vr_leg_holster_model_z_offset;

// vrbits0 flags, values taken from quakevr (quakedef_macros.hpp:299-310) so a
// progs.dat built against quakevr's QC reads the same bits.
#define QVR_VRBITS0_TELEPORTING			   (1u << 0)
#define QVR_VRBITS0_OFFHAND_GRABBING	   (1u << 1)
#define QVR_VRBITS0_OFFHAND_PREVGRABBING   (1u << 2)
#define QVR_VRBITS0_MAINHAND_GRABBING	   (1u << 3)
#define QVR_VRBITS0_MAINHAND_PREVGRABBING  (1u << 4)
#define QVR_VRBITS0_2H_AIMING			   (1u << 5)
#define QVR_VRBITS0_OFFHAND_RELOADING	   (1u << 6)
#define QVR_VRBITS0_OFFHAND_PREVRELOADING  (1u << 7)
#define QVR_VRBITS0_MAINHAND_RELOADING	   (1u << 8)
#define QVR_VRBITS0_MAINHAND_PREVRELOADING (1u << 9)

// Publishes hand state onto a player edict as QuakeC-readable fields, so game
// logic (holsters, throwing, force-grab) has something to work with.
struct edict_s;
void VR_XR_WriteEdictFields (struct edict_s *ed);

// Runs the hand-touch test between a player and a candidate entity, calling the
// target's .handtouch when a hand is inside it. This is how items get picked up
// by reaching for them rather than walking over them, and it is the entry point
// every grab-based system in quakevr's QC hangs off.
// (quakevr world.cpp:448-503)
void VR_XR_HandTouch (struct edict_s *ent, struct edict_s *target);

// Teleport locomotion: point with the off hand, hold to aim, release to go.
// Runs once per frame from the host loop.
void VR_XR_UpdateTeleport (void);

extern cvar_t vr_teleport_enabled;
extern cvar_t vr_teleport_range;
extern cvar_t vr_gun_wall_collision;

// Holster hotspot ids. Values are quakevr's (quakedef_macros.hpp:286-295) --
// its QC compares against these numbers directly, so they are a contract.
#define QVR_HS_NONE					 0
#define QVR_HS_OFFHAND_2H_GRAB		 1
#define QVR_HS_MAINHAND_2H_GRAB		 2
#define QVR_HS_LEFT_SHOULDER_HOLSTER 3
#define QVR_HS_RIGHT_SHOULDER_HOLSTER 4
#define QVR_HS_LEFT_HIP_HOLSTER		 5
#define QVR_HS_RIGHT_HIP_HOLSTER	 6
#define QVR_HS_HAND_SWITCH			 7
#define QVR_HS_LEFT_UPPER_HOLSTER	 8
#define QVR_HS_RIGHT_UPPER_HOLSTER	 9

extern cvar_t vr_shoulder_offset_x, vr_shoulder_offset_y, vr_shoulder_offset_z;
extern cvar_t vr_hip_offset_x, vr_hip_offset_y, vr_hip_offset_z, vr_hip_holster_thresh;
extern cvar_t vr_shoulder_holster_offset_x, vr_shoulder_holster_offset_y, vr_shoulder_holster_offset_z, vr_shoulder_holster_thresh;
extern cvar_t vr_upper_holster_offset_x, vr_upper_holster_offset_y, vr_upper_holster_offset_z, vr_upper_holster_thresh;

// Pulls the hand back so the muzzle stops at a wall instead of poking through
// it -- which otherwise lets the player shoot around corners from cover.
void VR_XR_ResolveGunCollision (vec3_t hand_pos, const vec3_t hand_angles, float muzzle_len);

// adds bonus reach for entities flagged easy to grab (quakevr serverdefines.hpp:54)
#define FL_EASYHANDTOUCH (1 << 13)

extern cvar_t vr_body_interactions;
extern cvar_t vr_throw_avg_frames;
extern cvar_t vr_throw_angvel_avg_frames;
extern cvar_t vr_weapon_throw_velocity_mult;
extern cvar_t vr_weapon_throw_damage_mult;
extern cvar_t vr_weapon_throw_mode;

// Clears throw velocity history, so a throw cannot inherit motion from before a
// respawn, teleport or level change.
void VR_XR_ResetThrowAvg (void);

#endif /* _VR_XR_H */

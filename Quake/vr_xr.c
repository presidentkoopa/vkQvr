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
static void VR_XR_Calibrate_f (void);
static void VR_XR_Recenter_f (void);

// Finger tracking cvars, declared here because VR_XR_Init registers them well
// before the finger-tracking section defines them. Defaults are quakevr's
// (vr_cvars.cpp:173-187).
cvar_t vr_finger_grip_bias = {"vr_finger_grip_bias", "0.0", CVAR_ARCHIVE};
cvar_t vr_finger_auto_close_thumb = {"vr_finger_auto_close_thumb", "1", CVAR_ARCHIVE};
cvar_t vr_finger_blending = {"vr_finger_blending", "1", CVAR_ARCHIVE};
cvar_t vr_finger_blending_speed = {"vr_finger_blending_speed", "50", CVAR_ARCHIVE};

// Room-scale bookkeeping. Head position in play space (Quake units) last frame,
// the delta to hand to the movement command, and the running total already
// turned into player movement -- the view subtracts that, otherwise a step
// forward moves both player and camera and you travel twice as far as you
// walked. (quakevr vr.cpp:2691-2692)
static vec3_t	xr_last_head_pos;
static vec3_t	xr_roomscale_consumed;
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
===============
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
===============
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
===============
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
===============
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

	Cvar_RegisterVariable (&vr_finger_grip_bias);
	Cvar_RegisterVariable (&vr_finger_auto_close_thumb);
	Cvar_RegisterVariable (&vr_finger_blending);
	Cvar_RegisterVariable (&vr_finger_blending_speed);

	// vkQuake ships gamma 0.9 / contrast 1.4, a brightened look tuned for a
	// monitor. quakevr runs both at 1 (gl_vidsdl.cpp:150-152); applied to a
	// headset, vkQuake's defaults lift the blacks and read as washed out.
	Cvar_SetQuick (&vid_gamma, "1");
	Cvar_SetQuick (&vid_contrast, "1");

	Cmd_AddCommand ("vr_calibrate", VR_XR_Calibrate_f);
	Cmd_AddCommand ("vr_recenter", VR_XR_Recenter_f);

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
================================================================================

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
===============
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
===============
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
===============
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
===============
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
================================================================================

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
===============
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
===============
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
===============
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
	else
		Con_Printf ("OpenXR: using STAGE space (room-scale)\n");

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
===============
VR_XR_SessionRunning
===============
*/
qboolean VR_XR_SessionRunning (void)
{
	return vr_xr_active && xr_session_running;
}

/*
===============
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
===============
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
===============
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
===============
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
================================================================================

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
cvar_t vr_world_scale = {"vr_world_scale", "1.0", CVAR_ARCHIVE};

// The runtime reports head position relative to the floor, but Quake's player
// origin sits at the middle of the bounding box, not the feet. quakevr settled
// on -16 units to reconcile the two. (quakevr vr_cvars.cpp:60)
cvar_t vr_floor_offset = {"vr_floor_offset", "-16", CVAR_ARCHIVE};

#define VR_METERS_TO_UNITS (vr_world_scale.value / (1.5f * 0.0254f))

/*
===============
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

	out_angles[1] = RAD2DEG (atan2f (fwd[1], fwd[0]));					   // yaw
	out_angles[0] = -RAD2DEG (asinf (CLAMP (-1.0f, fwd[2], 1.0f)));		   // pitch, inverted
	{
		// roll: how far "up" has rotated about the forward axis
		float sy = sinf (DEG2RAD (out_angles[1])), cy = cosf (DEG2RAD (out_angles[1]));
		float left[3] = {-sy, cy, 0.0f};
		float dot_left = up[0] * left[0] + up[1] * left[1] + up[2] * left[2];
		// negated: Quake's roll is positive tilting the other way to what the
		// up-vector projection gives us
		out_angles[2] = -RAD2DEG (asinf (CLAMP (-1.0f, dot_left, 1.0f)));
	}
}

/*
===============
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

	// take back whatever room-scale walking has already been handed to the
	// player as movement, or we would apply it twice
	out_offset[0] -= xr_roomscale_consumed[0];
	out_offset[1] -= xr_roomscale_consumed[1];

	// floor-relative height -> Quake's centre-of-box origin (quakevr vr.cpp:3516)
	out_offset[2] += vr_floor_offset.value;

	return true;
}

/*
===============
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
===============
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
===============
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
================================================================================

	INPUT

	The OpenXR action system is a layer of indirection over raw buttons: we
	declare abstract actions, suggest per-controller bindings, and the runtime
	resolves them. Users can then rebind in their runtime's own UI, and
	controllers we never tested still work if their profile is close enough.

	Action inventory mirrors quakevr's (vr.cpp:1204-1229), minus the menu set.

================================================================================
*/

vr_hand_t vr_xr_hand[VR_HANDS];

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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
===============
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
	out_pos[2] = loc.pose.position.y * scale + vr_floor_offset.value;

	XR_QuatToQuakeAngles (&loc.pose.orientation, out_angles);
	return true;
}

/*
================================================================================

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
===============
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
===============
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
===============
XR_UpdateFingers

Blending is quakevr's: move toward the target at a fixed rate rather than
snapping, so fingers do not pop between frames. (vr.cpp:3308-3341)
===============
*/
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
===============
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

	for (hand = 0; hand < VR_HANDS; hand++)
	{
		vr_hand_t *h = &vr_xr_hand[hand];

		// located at predicted display time, like the head, so hands and view
		// agree rather than drifting apart by a frame
		h->tracked = XR_LocateHand (xr_pose_space[hand], xr_frame_state.predictedDisplayTime, h->pos, h->angles, h->velocity);
		XR_LocateHand (xr_aim_space[hand], xr_frame_state.predictedDisplayTime, h->aim_pos, h->aim_angles, NULL);
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
}

/*
================================================================================

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
cvar_t vr_aim_hand = {"vr_aim_hand", "1", CVAR_ARCHIVE}; // 1 = right

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
cvar_t vr_height_calibration = {"vr_height_calibration", "1.6", CVAR_ARCHIVE};

// Global gun model tweaks, on top of the per-weapon table.
cvar_t vr_gunmodelscale = {"vr_gunmodelscale", "1.0", CVAR_ARCHIVE};
cvar_t vr_gunmodely = {"vr_gunmodely", "0", CVAR_ARCHIVE};

// VR body and leg holsters. Defaults are quakevr's (vr_cvars.cpp:123-145).
cvar_t vr_vrtorso_enabled = {"vr_vrtorso_enabled", "1", CVAR_ARCHIVE};
cvar_t vr_vrtorso_x_offset = {"vr_vrtorso_x_offset", "-3.25", CVAR_ARCHIVE};
cvar_t vr_vrtorso_y_offset = {"vr_vrtorso_y_offset", "0.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_z_offset = {"vr_vrtorso_z_offset", "-21.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_head_z_mult = {"vr_vrtorso_head_z_mult", "32.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_x_scale = {"vr_vrtorso_x_scale", "1.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_y_scale = {"vr_vrtorso_y_scale", "1.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_z_scale = {"vr_vrtorso_z_scale", "1.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_pitch = {"vr_vrtorso_pitch", "0.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_yaw = {"vr_vrtorso_yaw", "0.0", CVAR_ARCHIVE};
cvar_t vr_vrtorso_roll = {"vr_vrtorso_roll", "0.0", CVAR_ARCHIVE};

cvar_t vr_leg_holster_model_enabled = {"vr_leg_holster_model_enabled", "1", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_scale = {"vr_leg_holster_model_scale", "1", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_x_offset = {"vr_leg_holster_model_x_offset", "0.0", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_y_offset = {"vr_leg_holster_model_y_offset", "0.0", CVAR_ARCHIVE};
cvar_t vr_leg_holster_model_z_offset = {"vr_leg_holster_model_z_offset", "0.0", CVAR_ARCHIVE};

/*
================================================================================

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
===============
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
===============
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

	// quakevr's scale correction: its offsets were authored against a 0.75
	// world scale, so they have to be rescaled to whatever is in use now
	correct = (vr_world_scale.value / 0.75f) * vr_gunmodelscale.value;

	w = VR_FindWpnOffset (model_name);
	if (!w || !VR_XR_SessionRunning () || !vr_hand_aiming.value)
	{
		// not a known weapon, or VR is not driving the gun: put it back
		for (i = 0; i < 3; i++)
		{
			hdr->scale[i] = hdr->original_scale[i];
			hdr->scale_origin[i] = hdr->original_scale_origin[i];
		}
		return;
	}

	for (i = 0; i < 3; i++)
		hdr->scale[i] = hdr->original_scale[i] * w->scale * correct;

	hdr->scale_origin[0] = (hdr->original_scale_origin[0] + w->ofs_x) * correct;
	hdr->scale_origin[1] = (hdr->original_scale_origin[1] + w->ofs_y) * correct;
	hdr->scale_origin[2] = (hdr->original_scale_origin[2] + w->ofs_z + vr_gunmodely.value) * correct;
}

/*
===============
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

	for (i = 0; i < 3; i++)
	{
		hdr->scale[i] = hdr->original_scale[i] * scale[i];
		hdr->scale_origin[i] = hdr->original_scale_origin[i] + offset[i];
	}
}

/*
===============
VR_XR_ModAllModels

quakevr's VR_ModAllModels: the weapons, plus the player's own body and the leg
holsters, which are ordinary models positioned by the QC.
(quakevr vr.cpp:1128-1152, 1180-1185)
===============
*/
void VR_XR_ModAllModels (void)
{
	vec3_t scale, offset;

	if (!vr_xr_active)
		return;

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

	VR_XR_ModAllWeapons ();
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
===============
XR_TwoHandedAim

Returns true and fills out_angles when the off hand is gripping close enough to
the main hand to count as supporting the weapon.
===============
*/
static qboolean XR_TwoHandedAim (int main_hand, vec3_t out_angles)
{
	const int	off_hand = main_hand ^ 1;
	vec3_t		dir;
	float		len;

	if (!vr_two_handed.value)
		return false;
	if (!vr_xr_hand[main_hand].tracked || !vr_xr_hand[off_hand].tracked)
		return false;
	if (vr_xr_hand[off_hand].grip < 0.5f)
		return false;

	VectorSubtract (vr_xr_hand[main_hand].pos, vr_xr_hand[off_hand].pos, dir);
	len = VectorLength (dir);

	// too far apart and the off hand is doing something else entirely
	if (len < 1.0f || len > vr_two_hand_dist.value)
		return false;

	// front hand supports, rear hand holds: the barrel runs off-hand -> main
	out_angles[YAW] = RAD2DEG (atan2f (dir[1], dir[0]));
	out_angles[PITCH] = -RAD2DEG (asinf (CLAMP (-1.0f, dir[2] / len, 1.0f)));
	out_angles[ROLL] = 0.0f;
	return true;
}

/*
===============
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
===============
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

	hand = vr_aim_hand.value ? VR_HAND_RIGHT : VR_HAND_LEFT;
	h = &vr_xr_hand[hand];
	if (!h->tracked)
		return false;

	// same rebase the view uses: drop whatever room-scale walking already moved
	// the player, so the gun does not drift away from them
	local[0] = h->pos[0] - xr_roomscale_consumed[0] + vr_gun_offset_x.value;
	local[1] = h->pos[1] - xr_roomscale_consumed[1] + vr_gun_offset_y.value;
	local[2] = h->pos[2] + vr_gun_offset_z.value;

	yaw = DEG2RAD (cl.viewangles[YAW]);
	s = sinf (yaw);
	c = cosf (yaw);

	out_origin[0] = player_origin[0] + local[0] * c - local[1] * s;
	out_origin[1] = player_origin[1] + local[0] * s + local[1] * c;
	out_origin[2] = player_origin[2] + local[2];

	// the aim ray is what the weapon should point along, not the grip
	VectorCopy (h->aim_angles, out_angles);
	out_angles[YAW] += cl.viewangles[YAW];
	// viewmodels are drawn with inverted pitch, matching CalcGunAngle
	out_angles[PITCH] = -out_angles[PITCH];
	return true;
}

/*
===============
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

	hand = vr_aim_hand.value ? VR_HAND_RIGHT : VR_HAND_LEFT;
	if (!vr_xr_hand[hand].tracked)
		return false;

	if (!XR_TwoHandedAim (hand, out_angles))
		VectorCopy (vr_xr_hand[hand].aim_angles, out_angles);

	// hand angles are in play space; the body yaw the player has turned to with
	// the stick sits underneath, exactly as it does for the head
	out_angles[YAW] += cl.viewangles[YAW];
	return true;
}

/*
===============
VR_XR_UpdateLaser

A dynamic light at the impact point. Cheap, needs no new rendering path, and
reads as a laser dot because it lights the surface it lands on.
===============
*/
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
===============
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

		xr_roomscale_consumed[0] += xr_roomscale_delta[0];
		xr_roomscale_consumed[1] += xr_roomscale_delta[1];
	}

	VectorCopy (head, xr_last_head_pos);
}

/*
===============
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
===============
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
===============
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
===============
VR_XR_Buttons
===============
*/
unsigned int VR_XR_Buttons (void)
{
	static qboolean was_firing = false;
	static qboolean melee_armed = true;
	static int		last_health = 0;

	unsigned int bits = 0;
	int			 aim = vr_aim_hand.value ? VR_HAND_RIGHT : VR_HAND_LEFT;
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
===============
VR_XR_Impulse

Off-hand stick left/right cycles weapons. Edge triggered: one weapon per push,
and the stick has to return near centre before it fires again.
===============
*/
int VR_XR_Impulse (void)
{
	static qboolean armed = true;
	const int		off_hand = vr_aim_hand.value ? VR_HAND_LEFT : VR_HAND_RIGHT;
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
===============
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
===============
VR_XR_Recenter_f

Forgets the room-scale movement consumed so far, which re-anchors the player to
wherever they are physically standing now.
===============
*/
static void VR_XR_Recenter_f (void)
{
	xr_roomscale_consumed[0] = xr_roomscale_consumed[1] = xr_roomscale_consumed[2] = 0.0f;
	xr_head_pos_valid = false;
	Con_Printf ("vr_recenter: play space re-anchored\n");
}

/*
================================================================================

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
===============
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
===============
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

	local[0] = h->pos[0] - xr_roomscale_consumed[0];
	local[1] = h->pos[1] - xr_roomscale_consumed[1];
	local[2] = h->pos[2];

	out_pos[0] = player_origin[0] + local[0] * c - local[1] * s;
	out_pos[1] = player_origin[1] + local[0] * s + local[1] * c;
	out_pos[2] = player_origin[2] + local[2];

	VectorCopy (h->angles, out_angles);
	out_angles[YAW] += cl.viewangles[YAW];

	out_vel[0] = h->velocity[0] * c - h->velocity[1] * s;
	out_vel[1] = h->velocity[0] * s + h->velocity[1] * c;
	out_vel[2] = h->velocity[2];
}

/*
===============
VR_XR_WriteEdictFields
===============
*/
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

	main_hand = vr_aim_hand.value ? VR_HAND_RIGHT : VR_HAND_LEFT;
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
		XR_HandVelToWorld (&vr_xr_hand[main_hand], vr_xr_hand[main_hand].throw_velocity, tv);
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
		if (vr_xr_hand[off_hand].grip > 0.5f)
			bits += (float)QVR_VRBITS0_OFFHAND_GRABBING;
		if (vr_xr_hand[main_hand].grip > 0.5f)
			bits += (float)QVR_VRBITS0_MAINHAND_GRABBING;
		{
			vec3_t unused;
			if (XR_TwoHandedAim (main_hand, unused))
				bits += (float)QVR_VRBITS0_2H_AIMING;
		}
		XR_SetFloat (ed, f->vrbits0, bits);
	}

	XR_SetFloat (ed, f->ishuman, 1.0f);
}

/*
===============
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
===============
VR_XR_Haptic
===============
*/
void VR_XR_Haptic (int hand, float duration, float frequency, float amplitude)
{
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

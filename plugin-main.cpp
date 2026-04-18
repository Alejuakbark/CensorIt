#include <obs-module.h>
#include <obs-source.h>

#include "detect-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs_license_plate_blur", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Blur automotive license plates on async video (YOLOv8-style ONNX via OpenCV DNN). "
	       "Regional plate profiles, optional frame delay for heavier detection, and configurable blur. "
	       "Build against the same OBS major.minor libobs SDK as the OBS version you run.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "obs_license_plate_blur";
}

extern struct obs_source_info g_license_plate_blur_filter;

extern "C" MODULE_EXPORT bool obs_module_load(void)
{
	obs_register_source(&g_license_plate_blur_filter);
	return true;
}

extern "C" MODULE_EXPORT void obs_module_unload(void) {}

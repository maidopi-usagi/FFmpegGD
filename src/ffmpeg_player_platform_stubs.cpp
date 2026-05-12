#include "ffmpeg_player.h"

#ifndef FFMPEGGD_HAS_VIDEOTOOLBOX
bool FFmpegPlayer::init_videotoolbox_context() {
	return false;
}

bool FFmpegPlayer::upload_videotoolbox_frame(AVFrame *src_frame) {
	return false;
}

void FFmpegPlayer::cleanup_videotoolbox_resources() {
	using_godot_metal_device = false;
	cv_metal_texture_cache = nullptr;
	metal_nv12_to_rgba_pipeline = nullptr;
}
#endif

#if !defined(__linux__) && !((defined(_WIN32) || defined(WIN32)) && defined(FFMPEGGD_HAS_VULKAN))
bool FFmpegPlayer::init_vulkan_context() {
	return false;
}

void FFmpegPlayer::copy_vulkan_image(AVFrame *src_frame) {
}

void FFmpegPlayer::cleanup_vulkan_resources() {
	vk_command_pool = nullptr;
	vk_command_buffer = nullptr;
	vk_queue = nullptr;
	vk_device = nullptr;
	vk_phys_device = nullptr;
	vk_instance = nullptr;
	vk_queue_family_index = 0;
	using_godot_vulkan_device = false;
}
#endif

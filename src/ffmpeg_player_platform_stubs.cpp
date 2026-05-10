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

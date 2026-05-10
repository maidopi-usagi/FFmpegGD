#include "ffmpeg_player.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <chrono>
#include <cstring>
#include <cmath>

FFmpegPlayer::FFmpegPlayer() {
	texture_y.instantiate();
	texture_uv.instantiate();
	texture_v.instantiate();
	texture_rgba.instantiate();
	
	// Get RenderingDevice
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		rd = rs->get_rendering_device();
	}
}

FFmpegPlayer::~FFmpegPlayer() {
	cleanup();
}

void FFmpegPlayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_file", "path"), &FFmpegPlayer::load_file);
	ClassDB::bind_method(D_METHOD("play"), &FFmpegPlayer::play);
	ClassDB::bind_method(D_METHOD("stop"), &FFmpegPlayer::stop);
	ClassDB::bind_method(D_METHOD("is_playing"), &FFmpegPlayer::is_playing);
	ClassDB::bind_method(D_METHOD("set_debug_logging", "enabled"), &FFmpegPlayer::set_debug_logging);
	ClassDB::bind_method(D_METHOD("is_debug_logging_enabled"), &FFmpegPlayer::is_debug_logging_enabled);
	ClassDB::bind_method(D_METHOD("seek", "seconds"), &FFmpegPlayer::seek);
	ClassDB::bind_method(D_METHOD("get_duration"), &FFmpegPlayer::get_duration);
	ClassDB::bind_method(D_METHOD("get_position"), &FFmpegPlayer::get_position);
	ClassDB::bind_method(D_METHOD("has_audio"), &FFmpegPlayer::has_audio);
	ClassDB::bind_method(D_METHOD("get_audio_mix_rate"), &FFmpegPlayer::get_audio_mix_rate);
	ClassDB::bind_method(D_METHOD("get_queued_audio_frame_count"), &FFmpegPlayer::get_queued_audio_frame_count);
	ClassDB::bind_method(D_METHOD("pop_audio_frames", "max_frames"), &FFmpegPlayer::pop_audio_frames);
	ClassDB::bind_method(D_METHOD("get_video_texture_y"), &FFmpegPlayer::get_video_texture_y);
	ClassDB::bind_method(D_METHOD("get_video_texture_uv"), &FFmpegPlayer::get_video_texture_uv);
	ClassDB::bind_method(D_METHOD("get_video_texture_v"), &FFmpegPlayer::get_video_texture_v);
	ClassDB::bind_method(D_METHOD("get_video_texture_rgba"), &FFmpegPlayer::get_video_texture_rgba);
	ClassDB::bind_method(D_METHOD("get_video_size"), &FFmpegPlayer::get_video_size);
	ClassDB::bind_method(D_METHOD("is_yuv420p"), &FFmpegPlayer::is_yuv420p);
	ClassDB::bind_method(D_METHOD("get_video_colorspace"), &FFmpegPlayer::get_video_colorspace);
	ClassDB::bind_method(D_METHOD("get_video_color_range"), &FFmpegPlayer::get_video_color_range);
}

void FFmpegPlayer::cleanup() {
	stop_decode_thread();
	release_textures();
	cleanup_d3d12_resources();
	cleanup_videotoolbox_resources();
	cleanup_vulkan_resources();
	if (sws_ctx) {
		sws_freeContext(sws_ctx);
		sws_ctx = nullptr;
	}
	if (swr_ctx) {
		swr_free(&swr_ctx);
	}
	if (frame) {
		av_frame_free(&frame);
		frame = nullptr;
	}
	if (sw_frame) {
		av_frame_free(&sw_frame);
		sw_frame = nullptr;
	}
	if (audio_frame) {
		av_frame_free(&audio_frame);
		audio_frame = nullptr;
	}
	if (pkt) {
		av_packet_free(&pkt);
		pkt = nullptr;
	}
	if (audio_pending_pkt) {
		av_packet_free(&audio_pending_pkt);
		audio_pending_pkt = nullptr;
	}
	if (audio_decoder_ctx) {
		avcodec_free_context(&audio_decoder_ctx);
		audio_decoder_ctx = nullptr;
	}
	if (decoder_ctx) {
		avcodec_free_context(&decoder_ctx);
		decoder_ctx = nullptr;
	}
	if (fmt_ctx) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
	}
	if (hw_device_ctx) {
		av_buffer_unref(&hw_device_ctx);
		hw_device_ctx = nullptr;
	}
	playing = false;
	discard_decoded_frames_before = -1.0;
	discard_audio_frames_before = -1.0;
	resync_playback_on_next_frame = false;
	playback_generation++;
	playback_time = 0.0;
	is_planar_texture = false;
	logged_frame_format = false;
	logged_hw_frame_resource = false;
	logged_videotoolbox_frame_resource = false;
	current_video_colorspace = AVCOL_SPC_UNSPECIFIED;
	current_video_color_range = AVCOL_RANGE_UNSPECIFIED;
	decoded_frame_count = 0;
	stats_time_accum = 0.0;
	stats_decode_ms_accum = 0.0;
	stats_upload_ms_accum = 0.0;
	stats_frame_count = 0;
	packet_pending = false;
	audio_packet_pending = false;
	decoder_draining = false;
	audio_decoder_draining = false;
	audio_stream_idx = -1;
	audio_stream = nullptr;
	audio_output_sample_rate = 48000;
	logged_audio_format = false;
	{
		std::lock_guard<std::mutex> lock(decoded_frame_mutex);
		clear_decoded_frame(latest_decoded_frame);
		latest_decoded_frame = DecodedFrameData();
	}
	upload_y.resize(0);
	upload_u.resize(0);
	upload_v.resize(0);
	clear_audio_queue();
	hw_pix_fmt = AV_PIX_FMT_NONE;
	hw_device_type = AV_HWDEVICE_TYPE_NONE;
}

void FFmpegPlayer::release_textures() {
	cleanup_videotoolbox_resources();
	if (!rd) return;
	d3d12_textures_have_rendered = false;
	if (nv12_to_rgba_uniform_set_rid.is_valid()) {
		rd->free_rid(nv12_to_rgba_uniform_set_rid);
		nv12_to_rgba_uniform_set_rid = RID();
	}
	if (texture_y_rid.is_valid()) {
		rd->free_rid(texture_y_rid);
		texture_y_rid = RID();
		texture_y->set_texture_rd_rid(RID());
	}
	if (texture_uv_rid.is_valid()) {
		rd->free_rid(texture_uv_rid);
		texture_uv_rid = RID();
		texture_uv->set_texture_rd_rid(RID());
	}
	if (texture_v_rid.is_valid()) {
		rd->free_rid(texture_v_rid);
		texture_v_rid = RID();
		texture_v->set_texture_rd_rid(RID());
	}
	if (texture_rgba_rid.is_valid()) {
		rd->free_rid(texture_rgba_rid);
		texture_rgba_rid = RID();
		texture_rgba->set_texture_rd_rid(RID());
	}
}

void FFmpegPlayer::clear_decoded_frame(DecodedFrameData &decoded_frame) {
	if (decoded_frame.hw_frame) {
		av_frame_free(&decoded_frame.hw_frame);
	}
	decoded_frame = DecodedFrameData();
}

void FFmpegPlayer::clear_audio_queue() {
	std::lock_guard<std::mutex> lock(audio_mutex);
	audio_queue.clear();
}

void FFmpegPlayer::flush_audio_state() {
	if (audio_decoder_ctx) {
		avcodec_flush_buffers(audio_decoder_ctx);
	}
	if (audio_frame) {
		av_frame_unref(audio_frame);
	}
	if (audio_pending_pkt) {
		av_packet_unref(audio_pending_pkt);
	}
	audio_packet_pending = false;
	audio_decoder_draining = false;
	clear_audio_queue();
}

enum AVPixelFormat FFmpegPlayer::get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
	FFmpegPlayer *player = static_cast<FFmpegPlayer *>(ctx->opaque);
	const enum AVPixelFormat target = player ? player->hw_pix_fmt : AV_PIX_FMT_NONE;

	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == target) {
			return *p;
		}
	}

	UtilityFunctions::print("Requested HW surface format unavailable.");
	return AV_PIX_FMT_NONE;
}

int FFmpegPlayer::hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type) {
	int err = 0;
	if (hw_device_ctx) {
		av_buffer_unref(&hw_device_ctx);
	}

	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0)) < 0) {
		UtilityFunctions::print("Failed to create specified HW device.");
		return err;
	}
	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

	return err;
}

void FFmpegPlayer::load_file(const String &p_path) {
	cleanup();
	file_path = p_path.strip_edges();
	if ((file_path.begins_with("'") && file_path.ends_with("'")) || (file_path.begins_with("\"") && file_path.ends_with("\""))) {
		file_path = file_path.substr(1, file_path.length() - 2);
	}
	if (file_path.begins_with("res://") || file_path.begins_with("user://")) {
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		if (project_settings) {
			file_path = project_settings->globalize_path(file_path);
		}
	}
	if (open_media()) {
		UtilityFunctions::print("Media loaded successfully: ", file_path);
	} else {
		UtilityFunctions::print("Failed to load media: ", file_path);
	}
}

bool FFmpegPlayer::open_media() {
	const AVCodec *decoder = nullptr;
	const String rendering_driver = RenderingServer::get_singleton() ? RenderingServer::get_singleton()->get_current_rendering_driver_name() : String();

	// Open input file
	if (avformat_open_input(&fmt_ctx, file_path.utf8().get_data(), nullptr, nullptr) < 0) {
		UtilityFunctions::print("Could not open source file.");
		return false;
	}

	if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
		UtilityFunctions::print("Could not find stream information.");
		return false;
	}

	// Find video stream
	video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (video_stream_idx < 0) {
		UtilityFunctions::print("Could not find video stream.");
		return false;
	}
	video_stream = fmt_ctx->streams[video_stream_idx];

	// Godot's Vulkan device is not created with VK_KHR_video_decode_queue today,
	// so using it as FFmpeg's Vulkan decode device opens but fails at receive_frame().
	// Keep Vulkan copy code available for an engine-side video-queue path, but do not
	// select Vulkan decode from the public GDExtension path by default.
	const enum AVHWDeviceType hw_priority_d3d12[] = {
		AV_HWDEVICE_TYPE_D3D12VA,
		AV_HWDEVICE_TYPE_D3D11VA,
		AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_CUDA,
		AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_NONE
	};
	const enum AVHWDeviceType hw_priority_default[] = {
		AV_HWDEVICE_TYPE_D3D11VA,
		AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_CUDA,
		AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_NONE
	};
	const enum AVHWDeviceType hw_priority_apple_metal[] = {
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_NONE
	};
	const enum AVHWDeviceType *hw_priority = rendering_driver == "d3d12" ? hw_priority_d3d12 : hw_priority_default;
#if defined(__APPLE__)
	if (rendering_driver == "metal") {
		hw_priority = hw_priority_apple_metal;
	}
#endif
	if (debug_logging) {
		UtilityFunctions::print("Godot rendering driver: ", rendering_driver.is_empty() ? "unknown" : rendering_driver);
	}

	auto find_hw_config = [&](enum AVHWDeviceType p_type) -> const AVCodecHWConfig * {
		for (int i = 0;; i++) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
			if (!config) return nullptr;
			if (config->device_type == p_type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				return config;
			}
		}
	};

	auto try_open_decoder = [&](enum AVHWDeviceType p_type, enum AVPixelFormat p_hw_pix_fmt) -> bool {
		decoder_ctx = avcodec_alloc_context3(decoder);
		if (!decoder_ctx) return false;
		if (avcodec_parameters_to_context(decoder_ctx, video_stream->codecpar) < 0) {
			avcodec_free_context(&decoder_ctx);
			return false;
		}

		decoder_ctx->opaque = this;
		hw_device_type = p_type;
		hw_pix_fmt = p_hw_pix_fmt;

		if (p_type != AV_HWDEVICE_TYPE_NONE) {
			decoder_ctx->get_format = get_hw_format;
			if (p_type == AV_HWDEVICE_TYPE_D3D12VA && rendering_driver == "d3d12") {
				if (!init_d3d12_context(decoder_ctx)) {
					UtilityFunctions::print("Falling back to FFmpeg-created D3D12VA device.");
					if (hw_decoder_init(decoder_ctx, p_type) < 0) {
						avcodec_free_context(&decoder_ctx);
						return false;
					}
				}
			} else if (p_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
				if (hw_decoder_init(decoder_ctx, p_type) < 0) {
					avcodec_free_context(&decoder_ctx);
					return false;
				}
				if (rendering_driver == "metal" && !init_videotoolbox_context()) {
					UtilityFunctions::print("VideoToolbox will use CPU texture upload because Metal zero-copy import is unavailable.");
				}
			} else if (p_type == AV_HWDEVICE_TYPE_VULKAN) {
				if (!init_vulkan_context()) {
					avcodec_free_context(&decoder_ctx);
					return false;
				}
				decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
			} else if (hw_decoder_init(decoder_ctx, p_type) < 0) {
				avcodec_free_context(&decoder_ctx);
				return false;
			}
		}

		if (avcodec_open2(decoder_ctx, decoder, nullptr) == 0) {
			if (debug_logging && p_type == AV_HWDEVICE_TYPE_NONE) {
				UtilityFunctions::print("Using software decoding.");
			} else if (debug_logging) {
				UtilityFunctions::print("Using hardware acceleration: ", av_hwdevice_get_type_name(p_type));
			}
			return true;
		}

		UtilityFunctions::print("Failed to open decoder with ", p_type == AV_HWDEVICE_TYPE_NONE ? "software" : av_hwdevice_get_type_name(p_type), ".");
		avcodec_free_context(&decoder_ctx);
		if (hw_device_ctx) {
			av_buffer_unref(&hw_device_ctx);
		}
		if (p_type == AV_HWDEVICE_TYPE_VULKAN) {
			cleanup_vulkan_resources();
		}
		return false;
	};

	bool opened = false;
	for (const enum AVHWDeviceType *type = hw_priority; *type != AV_HWDEVICE_TYPE_NONE; type++) {
		const AVCodecHWConfig *config = find_hw_config(*type);
		if (!config) continue;
		opened = try_open_decoder(*type, config->pix_fmt);
		if (opened) break;
	}

	if (!opened) {
		opened = try_open_decoder(AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_NONE);
	}

	if (!opened) {
		UtilityFunctions::print("Failed to open codec.");
		return false;
	}

	frame = av_frame_alloc();
	sw_frame = av_frame_alloc();
	pkt = av_packet_alloc();
	open_audio_stream();

	return true;
}

void FFmpegPlayer::open_audio_stream() {
	const AVCodec *audio_decoder = nullptr;
	audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, video_stream_idx, &audio_decoder, 0);
	if (audio_stream_idx < 0 || !audio_decoder) {
		UtilityFunctions::print("No audio stream found.");
		return;
	}

	audio_stream = fmt_ctx->streams[audio_stream_idx];
	audio_decoder_ctx = avcodec_alloc_context3(audio_decoder);
	if (!audio_decoder_ctx) {
		UtilityFunctions::print("Could not allocate audio decoder context.");
		audio_stream_idx = -1;
		audio_stream = nullptr;
		return;
	}
	if (avcodec_parameters_to_context(audio_decoder_ctx, audio_stream->codecpar) < 0) {
		UtilityFunctions::print("Could not copy audio codec parameters.");
		avcodec_free_context(&audio_decoder_ctx);
		audio_stream_idx = -1;
		audio_stream = nullptr;
		return;
	}
	if (avcodec_open2(audio_decoder_ctx, audio_decoder, nullptr) < 0) {
		UtilityFunctions::print("Could not open audio decoder.");
		avcodec_free_context(&audio_decoder_ctx);
		audio_stream_idx = -1;
		audio_stream = nullptr;
		return;
	}

	audio_output_sample_rate = audio_decoder_ctx->sample_rate > 0 ? audio_decoder_ctx->sample_rate : 48000;
	audio_frame = av_frame_alloc();
	audio_pending_pkt = av_packet_alloc();
	if (!audio_frame || !audio_pending_pkt) {
		UtilityFunctions::print("Could not allocate audio frame or packet.");
		if (audio_frame) av_frame_free(&audio_frame);
		if (audio_pending_pkt) av_packet_free(&audio_pending_pkt);
		avcodec_free_context(&audio_decoder_ctx);
		audio_stream_idx = -1;
		audio_stream = nullptr;
		return;
	}

	AVChannelLayout in_layout = {};
	if (audio_decoder_ctx->ch_layout.nb_channels > 0) {
		av_channel_layout_copy(&in_layout, &audio_decoder_ctx->ch_layout);
	}
	if (in_layout.nb_channels <= 0) {
		av_channel_layout_default(&in_layout, audio_stream->codecpar->ch_layout.nb_channels > 0 ? audio_stream->codecpar->ch_layout.nb_channels : 2);
	}
	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, 2);
	if (swr_alloc_set_opts2(&swr_ctx, &out_layout, AV_SAMPLE_FMT_FLT, audio_output_sample_rate,
			&in_layout, audio_decoder_ctx->sample_fmt, audio_decoder_ctx->sample_rate, 0, nullptr) < 0 || swr_init(swr_ctx) < 0) {
		UtilityFunctions::print("Could not initialize audio resampler.");
		av_channel_layout_uninit(&in_layout);
		av_channel_layout_uninit(&out_layout);
		avcodec_free_context(&audio_decoder_ctx);
		av_frame_free(&audio_frame);
		av_packet_free(&audio_pending_pkt);
		audio_stream_idx = -1;
		audio_stream = nullptr;
		return;
	}
	av_channel_layout_uninit(&in_layout);
	av_channel_layout_uninit(&out_layout);
	if (debug_logging) {
		UtilityFunctions::print("Audio stream ready: ", audio_output_sample_rate, " Hz stereo float.");
	}
}

void FFmpegPlayer::play() {
	if (fmt_ctx && decoder_ctx) {
		playing = true;
		start_decode_thread();
	}
}

void FFmpegPlayer::stop() {
	stop_decode_thread();
}

bool FFmpegPlayer::is_playing() const {
	return playing;
}

void FFmpegPlayer::set_debug_logging(bool enabled) {
	debug_logging = enabled;
}

bool FFmpegPlayer::is_debug_logging_enabled() const {
	return debug_logging;
}

void FFmpegPlayer::seek(double seconds) {
	if (!fmt_ctx || !decoder_ctx || !video_stream) return;

	const bool should_resume = playing;
	stop_decode_thread();

	const double duration = get_duration();
	if (duration > 0.0) {
		seconds = CLAMP(seconds, 0.0, duration);
	} else if (seconds < 0.0) {
		seconds = 0.0;
	}

	const int64_t start_time = video_stream->start_time != AV_NOPTS_VALUE ? video_stream->start_time : 0;
	const int64_t timestamp = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, video_stream->time_base) + start_time;
	if (av_seek_frame(fmt_ctx, video_stream_idx, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
		UtilityFunctions::print("Failed to seek video.");
		return;
	}

	avcodec_flush_buffers(decoder_ctx);
	const uint64_t generation = playback_generation.fetch_add(1) + 1;
	if (pkt) av_packet_unref(pkt);
	if (frame) av_frame_unref(frame);
	if (sw_frame) av_frame_unref(sw_frame);
	packet_pending = false;
	decoder_draining = false;
	flush_audio_state();
	playback_time = seconds;
	discard_decoded_frames_before = seconds > 0.002 ? seconds - 0.002 : -1.0;
	discard_audio_frames_before = seconds > 0.002 ? seconds - 0.002 : -1.0;
	resync_playback_on_next_frame = true;
	stats_time_accum = 0.0;
	stats_upload_ms_accum = 0.0;
	stats_frame_count = 0;
	{
		std::lock_guard<std::mutex> lock(decoded_frame_mutex);
		clear_decoded_frame(latest_decoded_frame);
		latest_decoded_frame = DecodedFrameData();
		latest_decoded_frame.generation = generation;
	}

	if (should_resume) {
		playing = true;
		start_decode_thread();
	}
}

double FFmpegPlayer::get_duration() const {
	if (video_stream && video_stream->duration != AV_NOPTS_VALUE) {
		return video_stream->duration * av_q2d(video_stream->time_base);
	}
	if (fmt_ctx && fmt_ctx->duration != AV_NOPTS_VALUE) {
		return fmt_ctx->duration / (double)AV_TIME_BASE;
	}
	return 0.0;
}

double FFmpegPlayer::get_position() const {
	return playback_time;
}

bool FFmpegPlayer::has_audio() const {
	return audio_decoder_ctx != nullptr;
}

int FFmpegPlayer::get_audio_mix_rate() const {
	return audio_output_sample_rate;
}

int FFmpegPlayer::get_queued_audio_frame_count() const {
	std::lock_guard<std::mutex> lock(audio_mutex);
	return (int)audio_queue.size();
}

PackedVector2Array FFmpegPlayer::pop_audio_frames(int max_frames) {
	PackedVector2Array frames;
	if (max_frames <= 0) return frames;

	std::lock_guard<std::mutex> lock(audio_mutex);
	const int count = MIN(max_frames, (int)audio_queue.size());
	for (int i = 0; i < count; i++) {
		frames.push_back(audio_queue.front());
		audio_queue.pop_front();
	}
	return frames;
}

Ref<Texture2DRD> FFmpegPlayer::get_video_texture_y() const {
	if (!texture_y_rid.is_valid()) return Ref<Texture2DRD>();
	return texture_y;
}

Ref<Texture2DRD> FFmpegPlayer::get_video_texture_uv() const {
	if (!texture_uv_rid.is_valid()) return Ref<Texture2DRD>();
	return texture_uv;
}

Ref<Texture2DRD> FFmpegPlayer::get_video_texture_v() const {
	if (!texture_v_rid.is_valid()) return Ref<Texture2DRD>();
	return texture_v;
}

Ref<Texture2DRD> FFmpegPlayer::get_video_texture_rgba() const {
	if (!texture_rgba_rid.is_valid()) return Ref<Texture2DRD>();
	return texture_rgba;
}

Vector2i FFmpegPlayer::get_video_size() const {
	if (decoder_ctx) {
		return Vector2i(decoder_ctx->width, decoder_ctx->height);
	}
	return Vector2i(0, 0);
}

bool FFmpegPlayer::is_yuv420p() const {
	return is_planar_texture;
}

int FFmpegPlayer::get_video_colorspace() const {
	return current_video_colorspace;
}

int FFmpegPlayer::get_video_color_range() const {
	return current_video_color_range;
}

void FFmpegPlayer::_process(double delta) {
	if (!playing) return;

	const bool waiting_for_seek_frame = resync_playback_on_next_frame.load();
	if (!waiting_for_seek_frame) {
		playback_time += delta;
	}
	const double duration = get_duration();
	if (duration > 0.0 && playback_time >= duration) {
		playback_time = std::fmod(playback_time, duration);
	}
	stats_time_accum += delta;

	DecodedFrameData frame_to_upload;
	bool has_queued_frame = false;
	const uint64_t current_generation = playback_generation.load();
	{
		std::lock_guard<std::mutex> lock(decoded_frame_mutex);
		if (latest_decoded_frame.valid && latest_decoded_frame.generation == current_generation) {
			const double pts = latest_decoded_frame.pts_seconds;
			if (waiting_for_seek_frame || pts < 0.0 || pts <= playback_time + 0.002) {
				frame_to_upload = std::move(latest_decoded_frame);
				latest_decoded_frame = DecodedFrameData();
			}
		} else if (latest_decoded_frame.valid) {
			clear_decoded_frame(latest_decoded_frame);
			latest_decoded_frame = DecodedFrameData();
		}
		has_queued_frame = latest_decoded_frame.valid;
	}

	if (frame_to_upload.valid) {
		const auto upload_start = std::chrono::steady_clock::now();
		if (resync_playback_on_next_frame && frame_to_upload.pts_seconds >= 0.0) {
			playback_time = frame_to_upload.pts_seconds;
			resync_playback_on_next_frame = false;
		}
		current_video_colorspace = frame_to_upload.colorspace;
		current_video_color_range = frame_to_upload.color_range;
		upload_frame(frame_to_upload);
		clear_decoded_frame(frame_to_upload);
		const auto upload_end = std::chrono::steady_clock::now();
		stats_upload_ms_accum += std::chrono::duration<double, std::milli>(upload_end - upload_start).count();
		stats_frame_count++;
	}

	if (debug_logging && stats_time_accum >= 1.0) {
		UtilityFunctions::print("Playback stats: frames=", decoded_frame_count,
			", fps=", stats_frame_count / stats_time_accum,
			", queued=", has_queued_frame,
			", avg_upload_ms=", stats_frame_count > 0 ? stats_upload_ms_accum / stats_frame_count : 0.0);
		stats_time_accum = 0.0;
		stats_upload_ms_accum = 0.0;
		stats_frame_count = 0;
	}
}

void FFmpegPlayer::start_decode_thread() {
	if (decode_thread_running || decode_thread.joinable()) return;
	decode_thread_running = true;
	decode_thread = std::thread(&FFmpegPlayer::decode_thread_main, this);
}

void FFmpegPlayer::stop_decode_thread() {
	decode_thread_running = false;
	playing = false;
	if (decode_thread.joinable()) {
		decode_thread.join();
	}
}

void FFmpegPlayer::decode_thread_main() {
	while (decode_thread_running) {
		const uint64_t frame_generation = playback_generation.load();
		drain_audio_frames();

		bool has_pending_frame = false;
		{
			std::lock_guard<std::mutex> lock(decoded_frame_mutex);
			has_pending_frame = latest_decoded_frame.valid;
		}
		if (has_pending_frame) {
			// Bound the queue to one frame. This keeps latency low and prevents
			// the worker from decoding ahead and replacing frames before display.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		DecodedFrameData decoded;
		if (!decode_next_frame(decoded)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (decoded.valid && decoded.generation == playback_generation.load()) {
			std::lock_guard<std::mutex> lock(decoded_frame_mutex);
			clear_decoded_frame(latest_decoded_frame);
			latest_decoded_frame = std::move(decoded);
		} else if (decoded.valid) {
			clear_decoded_frame(decoded);
		}
	}
}

void FFmpegPlayer::drain_audio_frames() {
	if (!audio_decoder_ctx || !audio_frame || !swr_ctx) return;

	while (decode_thread_running) {
		const uint64_t frame_generation = playback_generation.load();
		const int ret = avcodec_receive_frame(audio_decoder_ctx, audio_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
		if (ret < 0) {
			UtilityFunctions::print("Error receiving audio frame: ", ret);
			return;
		}

		if (debug_logging && !logged_audio_format) {
			UtilityFunctions::print("Decoded audio format: ", av_get_sample_fmt_name((AVSampleFormat)audio_frame->format),
				", sample_rate: ", audio_frame->sample_rate,
				", channels: ", audio_frame->ch_layout.nb_channels);
			logged_audio_format = true;
		}

		const double discard_audio_before = discard_audio_frames_before.load();
		if (discard_audio_before >= 0.0) {
			const int64_t audio_pts = audio_frame->best_effort_timestamp != AV_NOPTS_VALUE ? audio_frame->best_effort_timestamp : audio_frame->pts;
			if (audio_pts != AV_NOPTS_VALUE && audio_stream) {
				const int64_t start_time = audio_stream->start_time != AV_NOPTS_VALUE ? audio_stream->start_time : 0;
				const double frame_start = (audio_pts - start_time) * av_q2d(audio_stream->time_base);
				const double sample_rate = audio_frame->sample_rate > 0 ? audio_frame->sample_rate : audio_output_sample_rate;
				const double frame_end = frame_start + audio_frame->nb_samples / sample_rate;
				if (frame_end < discard_audio_before) {
					av_frame_unref(audio_frame);
					continue;
				}
				discard_audio_frames_before = -1.0;
			}
		}

		if (frame_generation != playback_generation.load()) {
			av_frame_unref(audio_frame);
			continue;
		}

		std::lock_guard<std::mutex> lock(audio_mutex);
		if (frame_generation != playback_generation.load()) {
			av_frame_unref(audio_frame);
			continue;
		}
		const size_t max_queue_frames = (size_t)audio_output_sample_rate * 5;
		if (audio_frame->format == AV_SAMPLE_FMT_FLTP && audio_frame->ch_layout.nb_channels > 0) {
			const float *left_src = (const float *)audio_frame->extended_data[0];
			const float *right_src = audio_frame->ch_layout.nb_channels > 1 ? (const float *)audio_frame->extended_data[1] : left_src;
			for (int i = 0; i < audio_frame->nb_samples; i++) {
				if (audio_queue.size() >= max_queue_frames) break;
				float left = left_src[i];
				float right = right_src[i];
				if (!std::isfinite(left)) left = 0.0f;
				if (!std::isfinite(right)) right = 0.0f;
				left = left < -1.0f ? -1.0f : (left > 1.0f ? 1.0f : left);
				right = right < -1.0f ? -1.0f : (right > 1.0f ? 1.0f : right);
				audio_queue.push_back(Vector2(left, right));
			}
			av_frame_unref(audio_frame);
			continue;
		}

		if (audio_frame->format == AV_SAMPLE_FMT_FLT && audio_frame->ch_layout.nb_channels > 0) {
			const float *src = (const float *)audio_frame->extended_data[0];
			const int channels = audio_frame->ch_layout.nb_channels;
			for (int i = 0; i < audio_frame->nb_samples; i++) {
				if (audio_queue.size() >= max_queue_frames) break;
				float left = src[(size_t)i * channels];
				float right = channels > 1 ? src[(size_t)i * channels + 1] : left;
				if (!std::isfinite(left)) left = 0.0f;
				if (!std::isfinite(right)) right = 0.0f;
				left = left < -1.0f ? -1.0f : (left > 1.0f ? 1.0f : left);
				right = right < -1.0f ? -1.0f : (right > 1.0f ? 1.0f : right);
				audio_queue.push_back(Vector2(left, right));
			}
			av_frame_unref(audio_frame);
			continue;
		}

		const int out_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
		if (out_samples <= 0) {
			av_frame_unref(audio_frame);
			continue;
		}

		std::vector<float> interleaved((size_t)out_samples * 2);
		uint8_t *out_data[1] = { reinterpret_cast<uint8_t *>(interleaved.data()) };
		const int converted = swr_convert(swr_ctx, out_data, out_samples, (const uint8_t **)audio_frame->extended_data, audio_frame->nb_samples);
		av_frame_unref(audio_frame);
		if (converted <= 0) continue;

		for (int i = 0; i < converted; i++) {
			if (audio_queue.size() >= max_queue_frames) break;
			float left = interleaved[(size_t)i * 2];
			float right = interleaved[(size_t)i * 2 + 1];
			if (!std::isfinite(left)) left = 0.0f;
			if (!std::isfinite(right)) right = 0.0f;
			left = left < -1.0f ? -1.0f : (left > 1.0f ? 1.0f : left);
			right = right < -1.0f ? -1.0f : (right > 1.0f ? 1.0f : right);
			audio_queue.push_back(Vector2(left, right));
		}
	}
}

bool FFmpegPlayer::send_audio_packet(AVPacket *packet) {
	if (!audio_decoder_ctx) return true;
	drain_audio_frames();
	int ret = avcodec_send_packet(audio_decoder_ctx, packet);
	if (ret == 0 || ret == AVERROR_EOF) {
		drain_audio_frames();
		return true;
	}
	if (ret == AVERROR(EAGAIN)) {
		drain_audio_frames();
		ret = avcodec_send_packet(audio_decoder_ctx, packet);
		if (ret == 0 || ret == AVERROR_EOF) {
			drain_audio_frames();
			return true;
		}
	}
	UtilityFunctions::print("Error sending packet to audio decoder: ", ret);
	return false;
}

bool FFmpegPlayer::decode_next_frame(DecodedFrameData &out_frame) {
	int ret;
	
	while (decode_thread_running) {
		const uint64_t frame_generation = playback_generation.load();
		drain_audio_frames();
		if (audio_packet_pending && send_audio_packet(audio_pending_pkt)) {
			av_packet_unref(audio_pending_pkt);
			audio_packet_pending = false;
		}

		ret = avcodec_receive_frame(decoder_ctx, frame);
		if (ret == 0) {
			// We have a frame
			AVFrame *final_frame = frame;
			const int64_t raw_pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
			double frame_pts_seconds = -1.0;
			if (raw_pts != AV_NOPTS_VALUE) {
				const int64_t start_time = video_stream->start_time != AV_NOPTS_VALUE ? video_stream->start_time : 0;
				frame_pts_seconds = (raw_pts - start_time) * av_q2d(video_stream->time_base);
			}
			const double discard_before = discard_decoded_frames_before.load();
			if (discard_before >= 0.0 && frame_pts_seconds >= 0.0) {
				if (frame_pts_seconds < discard_before) {
					av_frame_unref(frame);
					continue;
				}
				discard_decoded_frames_before = -1.0;
			}
				
			if (frame->format == AV_PIX_FMT_VULKAN) {
				UtilityFunctions::print("Vulkan decoded frames are not supported by the worker upload path yet.");
				av_frame_unref(frame);
				continue;
			}

	if (frame->format == hw_pix_fmt) {
#ifdef FFMPEGGD_HAS_D3D12VA
				if (debug_logging && !logged_hw_frame_resource && frame->format == AV_PIX_FMT_D3D12) {
					AVD3D12VAFrame *d3d12_frame = (AVD3D12VAFrame *)frame->data[0];
					if (d3d12_frame) {
						UtilityFunctions::print("D3D12VA frame resource: ", (uint64_t)d3d12_frame->texture,
							", subresource=", d3d12_frame->subresource_index,
							", shared_godot_device=", using_godot_d3d12_device);
						logged_hw_frame_resource = true;
					}
				}
				if (frame->format == AV_PIX_FMT_D3D12 && using_godot_d3d12_device) {
					out_frame.width = frame->width;
					out_frame.height = frame->height;
					out_frame.colorspace = frame->colorspace;
					out_frame.color_range = frame->color_range;
					out_frame.pts_seconds = frame_pts_seconds;
					out_frame.generation = frame_generation;
					out_frame.hw_frame = av_frame_clone(frame);
					out_frame.valid = out_frame.hw_frame != nullptr;
					decoded_frame_count++;
					av_frame_unref(frame);
					return out_frame.valid;
				}
#endif

#ifdef FFMPEGGD_HAS_VIDEOTOOLBOX
				if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX && using_godot_metal_device) {
					if (debug_logging && !logged_videotoolbox_frame_resource) {
						UtilityFunctions::print("VideoToolbox frame resource: pixel_buffer=", (uint64_t)(uintptr_t)frame->data[3], ", zero_copy_metal=", using_godot_metal_device);
						logged_videotoolbox_frame_resource = true;
					}
					out_frame.width = frame->width;
					out_frame.height = frame->height;
					out_frame.colorspace = frame->colorspace;
					out_frame.color_range = frame->color_range;
					out_frame.pts_seconds = frame_pts_seconds;
					out_frame.generation = frame_generation;
					out_frame.hw_frame = av_frame_clone(frame);
					out_frame.valid = out_frame.hw_frame != nullptr;
					decoded_frame_count++;
					av_frame_unref(frame);
					return out_frame.valid;
				}
#endif

				// Retrieve data from GPU to CPU (NV12)
				if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
					UtilityFunctions::print("Error transferring the data to system memory: ", ret);
					av_frame_unref(frame);
					continue;
				}
				final_frame = sw_frame;
			}

			int width = final_frame->width;
			int height = final_frame->height;
			const bool is_yuv420p_frame = final_frame->format == AV_PIX_FMT_YUV420P || final_frame->format == AV_PIX_FMT_YUVJ420P;
			const bool is_nv12_frame = final_frame->format == AV_PIX_FMT_NV12;
			if (debug_logging && !logged_frame_format) {
				UtilityFunctions::print("Decoded frame format: ", av_get_pix_fmt_name((AVPixelFormat)final_frame->format),
					", sw_pix_fmt: ", av_get_pix_fmt_name(decoder_ctx->sw_pix_fmt),
					", color_range: ", final_frame->color_range,
					", colorspace: ", final_frame->colorspace,
					", linesizes: ", final_frame->linesize[0], ", ", final_frame->linesize[1], ", ", final_frame->linesize[2]);
				logged_frame_format = true;
			}
			if (!is_nv12_frame && !is_yuv420p_frame) {
				UtilityFunctions::print("Unsupported decoded pixel format: ", av_get_pix_fmt_name((AVPixelFormat)final_frame->format));
				av_frame_unref(frame);
				if (final_frame == sw_frame) av_frame_unref(sw_frame);
				continue;
			}

			out_frame.width = width;
			out_frame.height = height;
			out_frame.generation = frame_generation;
			out_frame.colorspace = final_frame->colorspace;
			out_frame.color_range = final_frame->color_range;
			const int64_t pts = final_frame->best_effort_timestamp != AV_NOPTS_VALUE ? final_frame->best_effort_timestamp : final_frame->pts;
			if (pts != AV_NOPTS_VALUE) {
				const int64_t start_time = video_stream->start_time != AV_NOPTS_VALUE ? video_stream->start_time : 0;
				out_frame.pts_seconds = (pts - start_time) * av_q2d(video_stream->time_base);
			}
			out_frame.y.resize(width * height);
			out_frame.u.resize(width * height / 4);
			out_frame.v.resize(width * height / 4);

			for (int i = 0; i < height; i++) {
				memcpy(out_frame.y.data() + i * width, final_frame->data[0] + i * final_frame->linesize[0], width);
			}

			if (is_yuv420p_frame) {
				for (int i = 0; i < height / 2; i++) {
					memcpy(out_frame.u.data() + i * (width / 2), final_frame->data[1] + i * final_frame->linesize[1], width / 2);
					memcpy(out_frame.v.data() + i * (width / 2), final_frame->data[2] + i * final_frame->linesize[2], width / 2);
				}
			} else {
				for (int y = 0; y < height / 2; y++) {
					const uint8_t *src_uv = final_frame->data[1] + y * final_frame->linesize[1];
					uint8_t *dst_u = out_frame.u.data() + y * (width / 2);
					uint8_t *dst_v = out_frame.v.data() + y * (width / 2);
					for (int x = 0; x < width / 2; x++) {
						dst_u[x] = src_uv[x * 2];
						dst_v[x] = src_uv[x * 2 + 1];
					}
				}
			}
			out_frame.valid = true;
			decoded_frame_count++;

			av_frame_unref(frame); // Unref the original frame
			if (final_frame == sw_frame) av_frame_unref(sw_frame);
			
			return true;
		}

		if (ret == AVERROR_EOF) {
			if (decoder_draining) {
				const int64_t start_time = video_stream->start_time != AV_NOPTS_VALUE ? video_stream->start_time : 0;
				const int seek_ret = av_seek_frame(fmt_ctx, video_stream_idx, start_time, AVSEEK_FLAG_BACKWARD);
				if (seek_ret < 0) {
					UtilityFunctions::print("Failed to loop video after EOF: ", seek_ret);
					playing = false;
					decode_thread_running = false;
					return false;
				}
				avcodec_flush_buffers(decoder_ctx);
				av_packet_unref(pkt);
				packet_pending = false;
				decoder_draining = false;
				flush_audio_state();
				continue;
			}
			playing = false;
			decode_thread_running = false;
			return false;
		}

		if (ret != AVERROR(EAGAIN)) {
			UtilityFunctions::print("Error receiving decoded frame: ", ret);
			playing = false;
			decode_thread_running = false;
			return false;
		}

		if (!packet_pending) {
			ret = av_read_frame(fmt_ctx, pkt);
			if (ret == AVERROR_EOF) {
				if (audio_decoder_ctx && !audio_decoder_draining) {
					send_audio_packet(nullptr);
					audio_decoder_draining = true;
				}
				ret = avcodec_send_packet(decoder_ctx, nullptr);
				if (ret == 0 || ret == AVERROR_EOF) {
					decoder_draining = true;
					continue;
				}
				if (ret != AVERROR(EAGAIN)) {
					UtilityFunctions::print("Error starting decoder drain: ", ret);
					playing = false;
					decode_thread_running = false;
					return false;
				}
				continue;
			}
			if (ret < 0) {
				UtilityFunctions::print("Error reading media packet: ", ret);
				playing = false;
				decode_thread_running = false;
				return false;
			}

			if (pkt->stream_index == audio_stream_idx && audio_decoder_ctx) {
				if (!send_audio_packet(pkt)) {
					av_packet_unref(pkt);
					continue;
				}
				av_packet_unref(pkt);
				continue;
			}

			if (pkt->stream_index != video_stream_idx) {
				av_packet_unref(pkt);
				continue;
			}
			packet_pending = true;
		}

		ret = avcodec_send_packet(decoder_ctx, pkt);
		if (ret == 0) {
			av_packet_unref(pkt);
			packet_pending = false;
			continue;
		}
		if (ret == AVERROR(EAGAIN)) {
			continue;
		}

		UtilityFunctions::print("Error sending packet to decoder: ", ret);
		av_packet_unref(pkt);
		packet_pending = false;
		playing = false;
		decode_thread_running = false;
		return false;
	}
	return false;
}

void FFmpegPlayer::upload_frame(const DecodedFrameData &decoded_frame) {
	if (!rd || !decoded_frame.valid) return;
	if (decoded_frame.hw_frame) {
		if (upload_d3d12_frame(decoded_frame.hw_frame)) {
			return;
		}
		if (upload_videotoolbox_frame(decoded_frame.hw_frame)) {
			return;
		}
		UtilityFunctions::print("GPU texture import failed; dropping hardware frame.");
		return;
	}

	upload_cpu_frame(decoded_frame);
}

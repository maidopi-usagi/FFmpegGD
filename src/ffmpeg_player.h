#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#if __has_include(<vulkan/vulkan.h>)
#define FFMPEGGD_HAS_VULKAN_HEADERS 1
#include <vulkan/vulkan.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#if (defined(_WIN32) || defined(WIN32)) && __has_include(<libavutil/hwcontext_d3d12va.h>)
#define FFMPEGGD_HAS_D3D12VA 1
#include <libavutil/hwcontext_d3d12va.h>
#endif
#if defined(__APPLE__) && __has_include(<libavutil/hwcontext_videotoolbox.h>)
#define FFMPEGGD_HAS_VIDEOTOOLBOX 1
#include <libavutil/hwcontext_videotoolbox.h>
#endif
#if defined(FFMPEGGD_HAS_VULKAN_HEADERS) && __has_include(<libavutil/hwcontext_vulkan.h>)
#define FFMPEGGD_HAS_VULKAN 1
#include <libavutil/hwcontext_vulkan.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

using namespace godot;

class FFmpegPlayer : public Node {
	GDCLASS(FFmpegPlayer, Node)

private:
	String file_path;
	
	// RenderingDevice textures
	Ref<Texture2DRD> texture_y;
	Ref<Texture2DRD> texture_uv; // Used for UV (NV12) or U (YUV420P)
	Ref<Texture2DRD> texture_v;  // Used for V (YUV420P)
	Ref<Texture2DRD> texture_rgba;
	RID texture_y_rid;
	RID texture_uv_rid;
	RID texture_v_rid;
	RID texture_rgba_rid;
	RID nv12_to_rgba_shader_rid;
	RID nv12_to_rgba_pipeline_rid;
	RID nv12_to_rgba_uniform_set_rid;
	PackedByteArray upload_y;
	PackedByteArray upload_u;
	PackedByteArray upload_v;
	RenderingDevice *rd = nullptr;

	// Vulkan resources
	void *vk_instance = nullptr;
	void *vk_phys_device = nullptr;
	void *vk_device = nullptr;
	void *vk_queue = nullptr;
	uint32_t vk_queue_family_index = 0;
	void *vk_command_pool = nullptr;
	void *vk_command_buffer = nullptr;
	bool using_godot_vulkan_device = false;
	bool using_godot_d3d12_device = false;
	bool d3d12_copy_initialized = false;
	bool d3d12_textures_have_rendered = false;
	bool using_godot_metal_device = false;
	void *d3d12_command_queue = nullptr;
	void *d3d12_command_allocator = nullptr;
	void *d3d12_command_list = nullptr;
	void *d3d12_copy_fence = nullptr;
	void *d3d12_copy_event = nullptr;
	void *d3d12_descriptor_heap = nullptr;
	void *d3d12_root_signature = nullptr;
	void *d3d12_compute_pipeline = nullptr;
	void *cv_metal_texture_cache = nullptr;
	void *metal_nv12_to_rgba_pipeline = nullptr;
	RID videotoolbox_rgba_rids[3];
	std::atomic<bool> videotoolbox_rgba_busy[3] = {};
	int videotoolbox_rgba_index = 0;
	int videotoolbox_rgba_width = 0;
	int videotoolbox_rgba_height = 0;
	uint64_t d3d12_copy_fence_value = 0;
	bool warned_zero_copy_unavailable = false;

	// FFmpeg variables
	AVFormatContext *fmt_ctx = nullptr;
	AVCodecContext *decoder_ctx = nullptr;
	AVCodecContext *audio_decoder_ctx = nullptr;
	AVBufferRef *hw_device_ctx = nullptr;
	int video_stream_idx = -1;
	int audio_stream_idx = -1;
	AVStream *video_stream = nullptr;
	AVStream *audio_stream = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *sw_frame = nullptr;
	AVFrame *audio_frame = nullptr;
	AVPacket *pkt = nullptr;
	AVPacket *audio_pending_pkt = nullptr;
	SwsContext *sws_ctx = nullptr;
	SwrContext *swr_ctx = nullptr;
	bool packet_pending = false;
	bool audio_packet_pending = false;
	bool decoder_draining = false;
	bool audio_decoder_draining = false;
	enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
	enum AVHWDeviceType hw_device_type = AV_HWDEVICE_TYPE_NONE;

	// Playback state
	std::atomic<bool> playing = false;
	std::atomic<double> discard_decoded_frames_before = -1.0;
	std::atomic<double> discard_audio_frames_before = -1.0;
	std::atomic<bool> resync_playback_on_next_frame = false;
	std::atomic<uint64_t> playback_generation = 0;
	double playback_time = 0.0;
	int audio_output_sample_rate = 48000;
	bool debug_logging = false;
	bool logged_audio_format = false;
	bool is_planar_texture = false;
	bool logged_frame_format = false;
	bool logged_hw_frame_resource = false;
	bool logged_videotoolbox_frame_resource = false;
	int current_video_colorspace = AVCOL_SPC_UNSPECIFIED;
	int current_video_color_range = AVCOL_RANGE_UNSPECIFIED;
	std::atomic<int64_t> decoded_frame_count = 0;
	double stats_time_accum = 0.0;
	double stats_decode_ms_accum = 0.0;
	double stats_upload_ms_accum = 0.0;
	int stats_frame_count = 0;

	struct DecodedFrameData {
		int width = 0;
		int height = 0;
		double pts_seconds = -1.0;
		std::vector<uint8_t> y;
		std::vector<uint8_t> u;
		std::vector<uint8_t> v;
		AVFrame *hw_frame = nullptr;
		int colorspace = AVCOL_SPC_UNSPECIFIED;
		int color_range = AVCOL_RANGE_UNSPECIFIED;
		uint64_t generation = 0;
		bool valid = false;
	};

	std::thread decode_thread;
	std::atomic<bool> decode_thread_running = false;
	std::mutex decoded_frame_mutex;
	DecodedFrameData latest_decoded_frame;
	mutable std::mutex audio_mutex;
	std::deque<Vector2> audio_queue;
	
	// Hardware acceleration
	static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
	int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
	bool init_d3d12_context(AVCodecContext *ctx);
	bool init_videotoolbox_context();
	bool init_d3d12_copy_resources();
	bool upload_d3d12_frame(AVFrame *src_frame);
	bool upload_videotoolbox_frame(AVFrame *src_frame);
	bool init_nv12_to_rgba_pipeline();
	bool dispatch_nv12_to_rgba(int width, int height);
	void cleanup_d3d12_resources();
	void cleanup_videotoolbox_resources();
	void clear_decoded_frame(DecodedFrameData &decoded_frame);
	bool init_vulkan_context();
	void copy_vulkan_image(AVFrame *src_frame);
	void cleanup_vulkan_resources();
	void release_textures();

	void cleanup();
	bool open_media();
	void open_audio_stream();
	void flush_audio_state();
	void clear_audio_queue();
	void start_decode_thread();
	void stop_decode_thread();
	void decode_thread_main();
	bool decode_next_frame(DecodedFrameData &out_frame);
	void drain_audio_frames();
	bool send_audio_packet(AVPacket *packet);
	void upload_frame(const DecodedFrameData &decoded_frame);
	void upload_cpu_frame(const DecodedFrameData &decoded_frame);

protected:
	static void _bind_methods();

public:
	FFmpegPlayer();
	~FFmpegPlayer();

	void _process(double delta) override;

	void load_file(const String &p_path);
	void play();
	void stop();
	bool is_playing() const;
	void set_debug_logging(bool enabled);
	bool is_debug_logging_enabled() const;
	void seek(double seconds);
	double get_duration() const;
	double get_position() const;
	bool has_audio() const;
	int get_audio_mix_rate() const;
	int get_queued_audio_frame_count() const;
	PackedVector2Array pop_audio_frames(int max_frames);
	Ref<Texture2DRD> get_video_texture_y() const;
	Ref<Texture2DRD> get_video_texture_uv() const;
	Ref<Texture2DRD> get_video_texture_v() const;
	Ref<Texture2DRD> get_video_texture_rgba() const;
	Vector2i get_video_size() const;
	bool is_yuv420p() const;
	int get_video_colorspace() const;
	int get_video_color_range() const;
};

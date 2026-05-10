#include "ffmpeg_player.h"

#ifdef FFMPEGGD_HAS_VIDEOTOOLBOX

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstdint>

namespace {

struct NV12ConversionParams {
	uint32_t width;
	uint32_t height;
	int32_t full_range;
	int32_t matrix;
	int32_t transfer;
	float source_peak_nits;
};

constexpr NSUInteger THREADGROUP_WIDTH = 16;
constexpr NSUInteger THREADGROUP_HEIGHT = 16;

} // namespace

bool FFmpegPlayer::init_videotoolbox_context() {
	if (cv_metal_texture_cache) {
		return true;
	}
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->get_rendering_device();
		}
	}
	if (!rd) {
		return false;
	}

	void *device_ptr = (void *)(uintptr_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	if (!device_ptr) {
		return false;
	}
	id<MTLDevice> metal_device = (__bridge id<MTLDevice>)device_ptr;
	CVMetalTextureCacheRef texture_cache = nullptr;
	const CVReturn ret = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, metal_device, nullptr, &texture_cache);
	if (ret != kCVReturnSuccess || !texture_cache) {
		return false;
	}

	cv_metal_texture_cache = texture_cache;
	using_godot_metal_device = true;
	if (debug_logging) {
		UtilityFunctions::print("Initialized VideoToolbox Metal zero-copy texture cache.");
	}
	return true;
}

void FFmpegPlayer::cleanup_videotoolbox_resources() {
	bool had_videotoolbox_rgba = false;
	if (rd) {
		for (int i = 0; i < 3; i++) {
			if (videotoolbox_rgba_rids[i].is_valid()) {
				had_videotoolbox_rgba = true;
				rd->free_rid(videotoolbox_rgba_rids[i]);
				videotoolbox_rgba_rids[i] = RID();
			}
			videotoolbox_rgba_busy[i] = false;
		}
	}
	videotoolbox_rgba_index = 0;
	videotoolbox_rgba_width = 0;
	videotoolbox_rgba_height = 0;
	if (had_videotoolbox_rgba) {
		texture_rgba_rid = RID();
		texture_rgba->set_texture_rd_rid(RID());
	}
	if (cv_metal_texture_cache) {
		CFRelease((CVMetalTextureCacheRef)cv_metal_texture_cache);
		cv_metal_texture_cache = nullptr;
	}
	if (metal_nv12_to_rgba_pipeline) {
		[(id<MTLComputePipelineState>)metal_nv12_to_rgba_pipeline release];
		metal_nv12_to_rgba_pipeline = nullptr;
	}
	using_godot_metal_device = false;
}

bool FFmpegPlayer::upload_videotoolbox_frame(AVFrame *src_frame) {
	if (!rd || !src_frame || src_frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !using_godot_metal_device) {
		return false;
	}
	if (!init_videotoolbox_context()) {
		return false;
	}

	CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)src_frame->data[3];
	if (!pixel_buffer) {
		return false;
	}

	const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
	const bool is_8bit_biplanar = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
	const bool is_10bit_biplanar = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
	if (!is_8bit_biplanar && !is_10bit_biplanar) {
		UtilityFunctions::print("Unsupported VideoToolbox pixel format for zero-copy import: ", (uint64_t)pixel_format);
		return false;
	}
	const MTLPixelFormat y_metal_format = is_10bit_biplanar ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
	const MTLPixelFormat uv_metal_format = is_10bit_biplanar ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;

	const size_t y_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 0);
	const size_t y_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 0);
	const size_t uv_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 1);
	const size_t uv_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 1);
	if (y_width == 0 || y_height == 0 || uv_width == 0 || uv_height == 0) {
		return false;
	}

	const bool replacing_plane_textures = texture_y_rid.is_valid() || texture_uv_rid.is_valid() || texture_v_rid.is_valid();
	const bool size_changed = videotoolbox_rgba_width != 0 && (videotoolbox_rgba_width != (int)y_width || videotoolbox_rgba_height != (int)y_height);
	if (replacing_plane_textures || size_changed) {
		release_textures();
		if (!init_videotoolbox_context()) {
			return false;
		}
	}

	CVMetalTextureRef y_cv_texture = nullptr;
	CVMetalTextureRef uv_cv_texture = nullptr;
	CVReturn ret = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
		(CVMetalTextureCacheRef)cv_metal_texture_cache,
		pixel_buffer,
		nullptr,
		y_metal_format,
		y_width,
		y_height,
		0,
		&y_cv_texture);
	if (ret != kCVReturnSuccess || !y_cv_texture) {
		return false;
	}
	ret = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
		(CVMetalTextureCacheRef)cv_metal_texture_cache,
		pixel_buffer,
		nullptr,
		uv_metal_format,
		uv_width,
		uv_height,
		1,
		&uv_cv_texture);
	if (ret != kCVReturnSuccess || !uv_cv_texture) {
		CFRelease(y_cv_texture);
		return false;
	}

	id<MTLTexture> y_texture = CVMetalTextureGetTexture(y_cv_texture);
	id<MTLTexture> uv_texture = CVMetalTextureGetTexture(uv_cv_texture);
	if (!y_texture || !uv_texture) {
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		return false;
	}

	if (!videotoolbox_rgba_rids[0].is_valid()) {
		Ref<RDTextureFormat> fmt_rgba;
		fmt_rgba.instantiate();
		fmt_rgba->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		fmt_rgba->set_width((int)y_width);
		fmt_rgba->set_height((int)y_height);
		fmt_rgba->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_rgba->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

		Ref<RDTextureView> view;
		view.instantiate();
		for (int i = 0; i < 3; i++) {
			videotoolbox_rgba_rids[i] = rd->texture_create(fmt_rgba, view);
			if (!videotoolbox_rgba_rids[i].is_valid()) {
				CFRelease(y_cv_texture);
				CFRelease(uv_cv_texture);
				cleanup_videotoolbox_resources();
				return false;
			}
			videotoolbox_rgba_busy[i] = false;
		}
		videotoolbox_rgba_width = (int)y_width;
		videotoolbox_rgba_height = (int)y_height;
		texture_rgba_rid = videotoolbox_rgba_rids[0];
		texture_rgba->set_texture_rd_rid(texture_rgba_rid);
	}

	int output_slot = -1;
	for (int i = 0; i < 3; i++) {
		const int slot = (videotoolbox_rgba_index + i) % 3;
		bool expected = false;
		if (videotoolbox_rgba_busy[slot].compare_exchange_strong(expected, true)) {
			output_slot = slot;
			break;
		}
	}
	if (output_slot < 0) {
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		return true;
	}
	videotoolbox_rgba_index = (output_slot + 1) % 3;
	RID output_rid = videotoolbox_rgba_rids[output_slot];

	id<MTLDevice> metal_device = (__bridge id<MTLDevice>)(void *)(uintptr_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>)(void *)(uintptr_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	id<MTLTexture> output_texture = (__bridge id<MTLTexture>)(void *)(uintptr_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, output_rid, 0);
	if (!metal_device || !command_queue || !output_texture) {
		videotoolbox_rgba_busy[output_slot] = false;
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		return false;
	}

	if (!metal_nv12_to_rgba_pipeline) {
		static NSString *shader_source = @"#include <metal_stdlib>\n"
			"using namespace metal;\n"
			"struct Params { uint width; uint height; int full_range; int matrix; int transfer; float source_peak_nits; };\n"
			"float3 pq_to_linear(float3 value) {\n"
			"    const float m1 = 0.1593017578125;\n"
			"    const float m2 = 78.84375;\n"
			"    const float c1 = 0.8359375;\n"
			"    const float c2 = 18.8515625;\n"
			"    const float c3 = 18.6875;\n"
			"    float3 v = pow(max(value, float3(0.0)), float3(1.0 / m2));\n"
			"    return pow(max(v - c1, float3(0.0)) / max(c2 - c3 * v, float3(0.000001)), float3(1.0 / m1));\n"
			"}\n"
			"float3 hlg_to_linear(float3 value) {\n"
			"    const float a = 0.17883277;\n"
			"    const float b = 0.28466892;\n"
			"    const float c = 0.55991073;\n"
			"    float3 low = (value * value) / 3.0;\n"
			"    float3 high = (exp((value - c) / a) + b) / 12.0;\n"
			"    return select(low, high, value > float3(0.5));\n"
			"}\n"
			"float3 map_hdr_nits(float3 linear, float source_peak_nits) {\n"
			"    const float target_peak_nits = 100.0;\n"
			"    float peak = max(source_peak_nits, target_peak_nits);\n"
			"    float shoulder = peak / target_peak_nits;\n"
			"    float3 nits = linear * 10000.0;\n"
			"    float3 x = nits / target_peak_nits;\n"
			"    float3 mapped = x * (1.0 + x / (shoulder * shoulder)) / (1.0 + x);\n"
			"    return pow(saturate(mapped), float3(1.0 / 2.2));\n"
			"}\n"
			"float3 tone_map(float3 rgb, int transfer, float source_peak_nits) {\n"
			"    if (transfer == 16) return map_hdr_nits(pq_to_linear(saturate(rgb)), source_peak_nits);\n"
			"    if (transfer == 18) return map_hdr_nits(hlg_to_linear(saturate(rgb)), source_peak_nits);\n"
			"    return rgb;\n"
			"}\n"
			"kernel void nv12_to_rgba(texture2d<float, access::read> y_tex [[texture(0)]], texture2d<float, access::read> uv_tex [[texture(1)]], texture2d<float, access::write> out_tex [[texture(2)]], constant Params &params [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
			"    if (gid.x >= params.width || gid.y >= params.height) return;\n"
			"    float y = y_tex.read(gid).r;\n"
			"    float2 uv = uv_tex.read(gid / 2).rg - float2(0.5, 0.5);\n"
			"    float rv; float gu; float gv; float bu;\n"
			"    if (params.matrix == 1) { rv = 1.5748; gu = -0.187324; gv = -0.468124; bu = 1.8556; }\n"
			"    else if (params.matrix == 9) { rv = 1.4746; gu = -0.164553; gv = -0.571353; bu = 1.8814; }\n"
			"    else { rv = 1.402; gu = -0.344136; gv = -0.714136; bu = 1.772; }\n"
			"    float yy = params.full_range != 0 ? y : 1.16438356 * (y - 0.0625);\n"
			"    float3 rgb = float3(yy + rv * uv.y, yy + gu * uv.x + gv * uv.y, yy + bu * uv.x);\n"
			"    out_tex.write(float4(saturate(tone_map(rgb, params.transfer, params.source_peak_nits)), 1.0), gid);\n"
			"}\n";
		NSError *error = nil;
		id<MTLLibrary> library = [metal_device newLibraryWithSource:shader_source options:nil error:&error];
		if (!library) {
			UtilityFunctions::print("Failed to compile Metal NV12 conversion shader: ", error ? String([[error localizedDescription] UTF8String]) : String());
			CFRelease(y_cv_texture);
			CFRelease(uv_cv_texture);
			return false;
		}
		id<MTLFunction> function = [library newFunctionWithName:@"nv12_to_rgba"];
		[library release];
		if (!function) {
			CFRelease(y_cv_texture);
			CFRelease(uv_cv_texture);
			return false;
		}
		id<MTLComputePipelineState> pipeline = [metal_device newComputePipelineStateWithFunction:function error:&error];
		[function release];
		if (!pipeline) {
			UtilityFunctions::print("Failed to create Metal NV12 conversion pipeline: ", error ? String([[error localizedDescription] UTF8String]) : String());
			CFRelease(y_cv_texture);
			CFRelease(uv_cv_texture);
			return false;
		}
		metal_nv12_to_rgba_pipeline = pipeline;
	}

	NV12ConversionParams params = {
		(uint32_t)y_width,
		(uint32_t)y_height,
		src_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
		(int32_t)src_frame->colorspace,
		(int32_t)src_frame->color_trc,
		current_hdr_source_peak_nits
	};

	id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
	if (!command_buffer) {
		videotoolbox_rgba_busy[output_slot] = false;
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		return false;
	}
	id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
	if (!encoder) {
		videotoolbox_rgba_busy[output_slot] = false;
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		return false;
	}
	[output_texture retain];
	std::atomic<bool> *output_busy = &videotoolbox_rgba_busy[output_slot];
	[encoder setComputePipelineState:(id<MTLComputePipelineState>)metal_nv12_to_rgba_pipeline];
	[encoder setTexture:y_texture atIndex:0];
	[encoder setTexture:uv_texture atIndex:1];
	[encoder setTexture:output_texture atIndex:2];
	[encoder setBytes:&params length:sizeof(params) atIndex:0];
	MTLSize threads_per_threadgroup = MTLSizeMake(THREADGROUP_WIDTH, THREADGROUP_HEIGHT, 1);
	MTLSize threadgroups = MTLSizeMake((y_width + THREADGROUP_WIDTH - 1) / THREADGROUP_WIDTH, (y_height + THREADGROUP_HEIGHT - 1) / THREADGROUP_HEIGHT, 1);
	[encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threads_per_threadgroup];
	[encoder endEncoding];
	[command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
		CFRelease(y_cv_texture);
		CFRelease(uv_cv_texture);
		output_busy->store(false);
		[output_texture release];
	}];
	[command_buffer commit];
	texture_rgba_rid = output_rid;
	texture_rgba->set_texture_rd_rid(texture_rgba_rid);

	if (texture_y_rid.is_valid()) {
		rd->free_rid(texture_y_rid);
		texture_y_rid = RID();
	}
	texture_y->set_texture_rd_rid(RID());
	if (texture_uv_rid.is_valid()) {
		rd->free_rid(texture_uv_rid);
		texture_uv_rid = RID();
	}
	texture_uv->set_texture_rd_rid(RID());
	if (texture_v_rid.is_valid()) {
		rd->free_rid(texture_v_rid);
		texture_v_rid = RID();
	}
	texture_v->set_texture_rd_rid(RID());
	is_planar_texture = false;
	return true;
}

#endif

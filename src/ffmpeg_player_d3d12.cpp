#include "ffmpeg_player.h"
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

#ifdef FFMPEGGD_HAS_D3D12VA
#include <d3dcompiler.h>
#endif

static unsigned int ffmpeggd_d3d12_calc_subresource(unsigned int mip_slice, unsigned int array_slice, unsigned int plane_slice, unsigned int mip_levels, unsigned int array_size) {
	return mip_slice + array_slice * mip_levels + plane_slice * mip_levels * array_size;
}

void FFmpegPlayer::cleanup_d3d12_resources() {
#ifdef FFMPEGGD_HAS_D3D12VA
	if (d3d12_copy_fence) {
		((ID3D12Fence *)d3d12_copy_fence)->Release();
	}
	if (d3d12_command_list) {
		((ID3D12GraphicsCommandList *)d3d12_command_list)->Release();
	}
	if (d3d12_command_allocator) {
		((ID3D12CommandAllocator *)d3d12_command_allocator)->Release();
	}
	if (d3d12_command_queue) {
		((ID3D12CommandQueue *)d3d12_command_queue)->Release();
	}
	if (d3d12_copy_event) {
		CloseHandle((HANDLE)d3d12_copy_event);
	}
	if (d3d12_compute_pipeline) {
		((ID3D12PipelineState *)d3d12_compute_pipeline)->Release();
	}
	if (d3d12_root_signature) {
		((ID3D12RootSignature *)d3d12_root_signature)->Release();
	}
	if (d3d12_descriptor_heap) {
		((ID3D12DescriptorHeap *)d3d12_descriptor_heap)->Release();
	}
#endif
	d3d12_command_queue = nullptr;
	d3d12_command_allocator = nullptr;
	d3d12_command_list = nullptr;
	d3d12_copy_fence = nullptr;
	d3d12_copy_event = nullptr;
	d3d12_descriptor_heap = nullptr;
	d3d12_root_signature = nullptr;
	d3d12_compute_pipeline = nullptr;
	d3d12_copy_fence_value = 0;
	d3d12_copy_initialized = false;
	d3d12_textures_have_rendered = false;
	if (rd) {
		if (nv12_to_rgba_uniform_set_rid.is_valid()) {
			rd->free_rid(nv12_to_rgba_uniform_set_rid);
			nv12_to_rgba_uniform_set_rid = RID();
		}
		if (yuv_linear_sampler_rid.is_valid()) {
			rd->free_rid(yuv_linear_sampler_rid);
			yuv_linear_sampler_rid = RID();
		}
		if (nv12_to_rgba_pipeline_rid.is_valid()) {
			rd->free_rid(nv12_to_rgba_pipeline_rid);
			nv12_to_rgba_pipeline_rid = RID();
		}
		if (nv12_to_rgba_shader_rid.is_valid()) {
			rd->free_rid(nv12_to_rgba_shader_rid);
			nv12_to_rgba_shader_rid = RID();
		}
	}
}

bool FFmpegPlayer::init_d3d12_copy_resources() {
#ifdef FFMPEGGD_HAS_D3D12VA
	if (d3d12_copy_initialized) return true;
	if (!rd) return false;

	ID3D12Device *device = (ID3D12Device *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	ID3D12CommandQueue *queue = (ID3D12CommandQueue *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	if (!device || !queue) {
		UtilityFunctions::print("Failed to get D3D12 device or command queue from Godot.");
		return false;
	}

	ID3D12CommandAllocator *allocator = nullptr;
	HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&allocator);
	if (FAILED(hr)) {
		UtilityFunctions::print("Failed to create D3D12 command allocator: ", (uint64_t)hr);
		return false;
	}

	ID3D12GraphicsCommandList *command_list = nullptr;
	hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&command_list);
	if (FAILED(hr)) {
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 command list: ", (uint64_t)hr);
		return false;
	}
	command_list->Close();

	ID3D12Fence *fence = nullptr;
	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
	if (FAILED(hr)) {
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 fence: ", (uint64_t)hr);
		return false;
	}

	HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!event) {
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 fence event.");
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = 3;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ID3D12DescriptorHeap *descriptor_heap = nullptr;
	hr = device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), (void **)&descriptor_heap);
	if (FAILED(hr)) {
		CloseHandle(event);
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 descriptor heap: ", (uint64_t)hr);
		return false;
	}

	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 2;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].RegisterSpace = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].RegisterSpace = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER root_parameters[2] = {};
	root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
	root_parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
	root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	root_parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
	root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC root_desc = {};
	root_desc.NumParameters = 2;
	root_desc.pParameters = root_parameters;
	root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	ID3DBlob *root_blob = nullptr;
	ID3DBlob *error_blob = nullptr;
	hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) error_blob->Release();
		descriptor_heap->Release();
		CloseHandle(event);
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to serialize D3D12 root signature: ", (uint64_t)hr);
		return false;
	}

	ID3D12RootSignature *root_signature = nullptr;
	hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&root_signature);
	root_blob->Release();
	if (error_blob) error_blob->Release();
	if (FAILED(hr)) {
		descriptor_heap->Release();
		CloseHandle(event);
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 root signature: ", (uint64_t)hr);
		return false;
	}

	static const char *nv12_to_rgba_hlsl =
		// TODO: Pass FFmpeg colorspace/range into this shader before validating D3D12 color output on Windows.
		"Texture2D<float> tex_y : register(t0);\n"
		"Texture2D<float2> tex_uv : register(t1);\n"
		"RWTexture2D<float4> tex_rgba : register(u0);\n"
		"[numthreads(16, 16, 1)]\n"
		"void main(uint3 id : SV_DispatchThreadID) {\n"
		"    uint w, h;\n"
		"    tex_rgba.GetDimensions(w, h);\n"
		"    if (id.x >= w || id.y >= h) return;\n"
		"    float y = tex_y.Load(int3(id.xy, 0));\n"
		"    float2 uv = tex_uv.Load(int3(id.xy / 2, 0));\n"
		"    float u = uv.x - 0.5;\n"
		"    float v = uv.y - 0.5;\n"
		"    float yy = 1.1643 * (y - 0.0625);\n"
		"    float3 rgb = float3(yy + 1.5748 * v, yy - 0.187324 * u - 0.468124 * v, yy + 1.8556 * u);\n"
		"    tex_rgba[id.xy] = float4(saturate(rgb), 1.0);\n"
		"}\n";
	ID3DBlob *shader_blob = nullptr;
	error_blob = nullptr;
	hr = D3DCompile(nv12_to_rgba_hlsl, strlen(nv12_to_rgba_hlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader_blob, &error_blob);
	if (FAILED(hr)) {
		String error_text = error_blob ? String((const char *)error_blob->GetBufferPointer()) : String();
		if (error_blob) error_blob->Release();
		root_signature->Release();
		descriptor_heap->Release();
		CloseHandle(event);
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to compile D3D12 NV12 to RGBA shader: ", error_text);
		return false;
	}
	if (error_blob) error_blob->Release();

	D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = root_signature;
	pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
	pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
	ID3D12PipelineState *pipeline = nullptr;
	hr = device->CreateComputePipelineState(&pso_desc, __uuidof(ID3D12PipelineState), (void **)&pipeline);
	shader_blob->Release();
	if (FAILED(hr)) {
		root_signature->Release();
		descriptor_heap->Release();
		CloseHandle(event);
		fence->Release();
		command_list->Release();
		allocator->Release();
		UtilityFunctions::print("Failed to create D3D12 NV12 to RGBA pipeline: ", (uint64_t)hr);
		return false;
	}

	queue->AddRef();
	d3d12_command_queue = queue;
	d3d12_command_allocator = allocator;
	d3d12_command_list = command_list;
	d3d12_copy_fence = fence;
	d3d12_copy_event = event;
	d3d12_descriptor_heap = descriptor_heap;
	d3d12_root_signature = root_signature;
	d3d12_compute_pipeline = pipeline;
	d3d12_copy_fence_value = 0;
	d3d12_copy_initialized = true;
	if (debug_logging) {
		UtilityFunctions::print("Initialized D3D12 GPU copy resources.");
	}
	return true;
#else
	return false;
#endif
}

bool FFmpegPlayer::init_nv12_to_rgba_pipeline() {
	if (nv12_to_rgba_pipeline_rid.is_valid()) return true;
	if (!rd) return false;

	static const char *shader_source = R"(#version 450

// TODO: Pass FFmpeg colorspace/range into this shader before validating RD compute color output.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D y_image;
layout(set = 0, binding = 1) uniform sampler2D uv_image;
layout(rgba8, set = 0, binding = 2) uniform writeonly image2D rgba_image;

void main() {
	ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = textureSize(y_image, 0);
	if (xy.x >= size.x || xy.y >= size.y) {
		return;
	}

	vec2 luma_size = vec2(textureSize(y_image, 0));
	vec2 sample_uv = (vec2(xy) + vec2(0.5)) / luma_size;
	float y = texelFetch(y_image, xy, 0).r;
	vec2 uv = texture(uv_image, sample_uv).rg;
	float u = uv.x - 0.5;
	float v = uv.y - 0.5;

	float yy = 1.1643 * (y - 0.0625);
	vec3 rgb = vec3(
		yy + 1.5748 * v,
		yy - 0.187324 * u - 0.468124 * v,
		yy + 1.8556 * u
	);
	imageStore(rgba_image, xy, vec4(clamp(rgb, 0.0, 1.0), 1.0));
}
)";

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, shader_source);
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	const String compile_error = spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("No SPIR-V returned.");
	if (!compile_error.is_empty()) {
		UtilityFunctions::print("Failed to compile NV12 to RGBA compute shader: ", compile_error);
		return false;
	}

	nv12_to_rgba_shader_rid = rd->shader_create_from_spirv(spirv);
	if (!nv12_to_rgba_shader_rid.is_valid()) {
		UtilityFunctions::print("Failed to create NV12 to RGBA compute shader.");
		return false;
	}

	nv12_to_rgba_pipeline_rid = rd->compute_pipeline_create(nv12_to_rgba_shader_rid);
	if (!nv12_to_rgba_pipeline_rid.is_valid()) {
		UtilityFunctions::print("Failed to create NV12 to RGBA compute pipeline.");
		return false;
	}

	if (debug_logging) {
		UtilityFunctions::print("Initialized RD NV12 to RGBA compute pipeline.");
	}
	return true;
}

bool FFmpegPlayer::dispatch_nv12_to_rgba(int width, int height) {
	if (!rd || !texture_y_rid.is_valid() || !texture_uv_rid.is_valid() || !texture_rgba_rid.is_valid()) return false;
	if (!init_nv12_to_rgba_pipeline()) return false;

	if (!nv12_to_rgba_uniform_set_rid.is_valid()) {
		TypedArray<RDUniform> uniforms;

		if (!yuv_linear_sampler_rid.is_valid()) {
			Ref<RDSamplerState> sampler_state;
			sampler_state.instantiate();
			sampler_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
			sampler_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
			sampler_state->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
			sampler_state->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
			sampler_state->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
			yuv_linear_sampler_rid = rd->sampler_create(sampler_state);
			if (!yuv_linear_sampler_rid.is_valid()) {
				UtilityFunctions::print("Failed to create YUV linear sampler.");
				return false;
			}
		}

		Ref<RDUniform> y_uniform;
		y_uniform.instantiate();
		y_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		y_uniform->set_binding(0);
		y_uniform->add_id(yuv_linear_sampler_rid);
		y_uniform->add_id(texture_y_rid);
		uniforms.push_back(y_uniform);

		Ref<RDUniform> uv_uniform;
		uv_uniform.instantiate();
		uv_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		uv_uniform->set_binding(1);
		uv_uniform->add_id(yuv_linear_sampler_rid);
		uv_uniform->add_id(texture_uv_rid);
		uniforms.push_back(uv_uniform);

		Ref<RDUniform> rgba_uniform;
		rgba_uniform.instantiate();
		rgba_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
		rgba_uniform->set_binding(2);
		rgba_uniform->add_id(texture_rgba_rid);
		uniforms.push_back(rgba_uniform);

		nv12_to_rgba_uniform_set_rid = rd->uniform_set_create(uniforms, nv12_to_rgba_shader_rid, 0);
		if (!nv12_to_rgba_uniform_set_rid.is_valid()) {
			UtilityFunctions::print("Failed to create NV12 to RGBA uniform set.");
			return false;
		}
	}

	const int64_t compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, nv12_to_rgba_pipeline_rid);
	rd->compute_list_bind_uniform_set(compute_list, nv12_to_rgba_uniform_set_rid, 0);
	rd->compute_list_dispatch(compute_list, (width + 15) / 16, (height + 15) / 16, 1);
	rd->compute_list_end();
	return true;
}

bool FFmpegPlayer::upload_d3d12_frame(AVFrame *src_frame) {
#ifdef FFMPEGGD_HAS_D3D12VA
	if (!rd || !src_frame || src_frame->format != AV_PIX_FMT_D3D12 || !using_godot_d3d12_device) return false;
	if (!init_d3d12_copy_resources()) return false;

	AVD3D12VAFrame *d3d12_frame = (AVD3D12VAFrame *)src_frame->data[0];
	if (!d3d12_frame || !d3d12_frame->texture) return false;

	if (d3d12_frame->sync_ctx.fence && d3d12_frame->sync_ctx.fence_value > d3d12_frame->sync_ctx.fence->GetCompletedValue()) {
		HANDLE wait_event = d3d12_frame->sync_ctx.event ? d3d12_frame->sync_ctx.event : (HANDLE)d3d12_copy_event;
		if (SUCCEEDED(d3d12_frame->sync_ctx.fence->SetEventOnCompletion(d3d12_frame->sync_ctx.fence_value, wait_event))) {
			WaitForSingleObject(wait_event, INFINITE);
		}
	}

	const int width = src_frame->width;
	const int height = src_frame->height;
	const bool recreate_textures = !texture_y_rid.is_valid() || texture_y->get_width() != width || texture_y->get_height() != height || !texture_uv_rid.is_valid() || !texture_rgba_rid.is_valid() || texture_v_rid.is_valid() || is_planar_texture;
	if (recreate_textures) {
		release_textures();

		Ref<RDTextureView> view;
		view.instantiate();

		Ref<RDTextureFormat> fmt_y;
		fmt_y.instantiate();
		fmt_y->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		fmt_y->set_width(width);
		fmt_y->set_height(height);
		fmt_y->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_y->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		texture_y_rid = rd->texture_create(fmt_y, view);
		texture_y->set_texture_rd_rid(texture_y_rid);

		Ref<RDTextureFormat> fmt_uv;
		fmt_uv.instantiate();
		fmt_uv->set_format(RenderingDevice::DATA_FORMAT_R8G8_UNORM);
		fmt_uv->set_width(width / 2);
		fmt_uv->set_height(height / 2);
		fmt_uv->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_uv->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		texture_uv_rid = rd->texture_create(fmt_uv, view);
		texture_uv->set_texture_rd_rid(texture_uv_rid);

		Ref<RDTextureFormat> fmt_rgba;
		fmt_rgba.instantiate();
		fmt_rgba->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		fmt_rgba->set_width(width);
		fmt_rgba->set_height(height);
		fmt_rgba->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_rgba->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		texture_rgba_rid = rd->texture_create(fmt_rgba, view);
		texture_rgba->set_texture_rd_rid(texture_rgba_rid);

		texture_v->set_texture_rd_rid(RID());
		is_planar_texture = false;
	}

	ID3D12Resource *dst_y = (ID3D12Resource *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, texture_y_rid, 0);
	ID3D12Resource *dst_uv = (ID3D12Resource *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, texture_uv_rid, 0);
	ID3D12Resource *dst_rgba = (ID3D12Resource *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, texture_rgba_rid, 0);
	if (!dst_y || !dst_uv || !dst_rgba) return false;

	ID3D12Resource *src = d3d12_frame->texture;
	const D3D12_RESOURCE_DESC src_desc = src->GetDesc();
	const UINT src_y_subresource = ffmpeggd_d3d12_calc_subresource(0, d3d12_frame->subresource_index, 0, 1, src_desc.DepthOrArraySize);
	const UINT src_uv_subresource = ffmpeggd_d3d12_calc_subresource(0, d3d12_frame->subresource_index, 1, 1, src_desc.DepthOrArraySize);

	ID3D12CommandAllocator *allocator = (ID3D12CommandAllocator *)d3d12_command_allocator;
	ID3D12GraphicsCommandList *command_list = (ID3D12GraphicsCommandList *)d3d12_command_list;
	ID3D12CommandQueue *queue = (ID3D12CommandQueue *)d3d12_command_queue;
	ID3D12Fence *fence = (ID3D12Fence *)d3d12_copy_fence;
	ID3D12DescriptorHeap *descriptor_heap = (ID3D12DescriptorHeap *)d3d12_descriptor_heap;
	ID3D12RootSignature *root_signature = (ID3D12RootSignature *)d3d12_root_signature;
	ID3D12PipelineState *pipeline = (ID3D12PipelineState *)d3d12_compute_pipeline;
	ID3D12Device *device = (ID3D12Device *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	if (!descriptor_heap || !root_signature || !pipeline || !device) return false;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
	const UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC y_srv = {};
	y_srv.Format = DXGI_FORMAT_R8_UNORM;
	y_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	y_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	y_srv.Texture2D.MipLevels = 1;
	y_srv.Texture2D.PlaneSlice = 0;
	device->CreateShaderResourceView(dst_y, &y_srv, cpu_handle);
	cpu_handle.ptr += descriptor_size;
	D3D12_SHADER_RESOURCE_VIEW_DESC uv_srv = {};
	uv_srv.Format = DXGI_FORMAT_R8G8_UNORM;
	uv_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	uv_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	uv_srv.Texture2D.MipLevels = 1;
	uv_srv.Texture2D.PlaneSlice = 0;
	device->CreateShaderResourceView(dst_uv, &uv_srv, cpu_handle);
	cpu_handle.ptr += descriptor_size;
	D3D12_UNORDERED_ACCESS_VIEW_DESC rgba_uav = {};
	rgba_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rgba_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(dst_rgba, nullptr, &rgba_uav, cpu_handle);

	if (FAILED(allocator->Reset()) || FAILED(command_list->Reset(allocator, nullptr))) return false;

	D3D12_RESOURCE_BARRIER barriers[6] = {};
	UINT barrier_count = 0;
	barriers[barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[barrier_count].Transition.pResource = src;
	barriers[barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier_count++;
	if (d3d12_textures_have_rendered) {
		barriers[barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[barrier_count].Transition.pResource = dst_y;
		barriers[barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier_count++;
		barriers[barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[barrier_count].Transition.pResource = dst_uv;
		barriers[barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier_count++;
		barriers[barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[barrier_count].Transition.pResource = dst_rgba;
		barriers[barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier_count++;
	} else {
		barriers[barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[barrier_count].Transition.pResource = dst_rgba;
		barriers[barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier_count++;
	}
	command_list->ResourceBarrier(barrier_count, barriers);

	D3D12_TEXTURE_COPY_LOCATION src_y = {};
	src_y.pResource = src;
	src_y.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_y.SubresourceIndex = src_y_subresource;
	D3D12_TEXTURE_COPY_LOCATION dst_y_loc = {};
	dst_y_loc.pResource = dst_y;
	dst_y_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_y_loc.SubresourceIndex = 0;
	D3D12_BOX y_box = { 0, 0, 0, (UINT)width, (UINT)height, 1 };
	command_list->CopyTextureRegion(&dst_y_loc, 0, 0, 0, &src_y, &y_box);

	D3D12_TEXTURE_COPY_LOCATION src_uv = {};
	src_uv.pResource = src;
	src_uv.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_uv.SubresourceIndex = src_uv_subresource;
	D3D12_TEXTURE_COPY_LOCATION dst_uv_loc = {};
	dst_uv_loc.pResource = dst_uv;
	dst_uv_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_uv_loc.SubresourceIndex = 0;
	D3D12_BOX uv_box = { 0, 0, 0, (UINT)(width / 2), (UINT)(height / 2), 1 };
	command_list->CopyTextureRegion(&dst_uv_loc, 0, 0, 0, &src_uv, &uv_box);

	D3D12_RESOURCE_BARRIER end_barriers[4] = {};
	UINT end_barrier_count = 0;
	end_barriers[end_barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	end_barriers[end_barrier_count].Transition.pResource = src;
	end_barriers[end_barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	end_barriers[end_barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	end_barriers[end_barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	end_barrier_count++;
	end_barriers[end_barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	end_barriers[end_barrier_count].Transition.pResource = dst_y;
	end_barriers[end_barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	end_barriers[end_barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	end_barriers[end_barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	end_barrier_count++;
	end_barriers[end_barrier_count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	end_barriers[end_barrier_count].Transition.pResource = dst_uv;
	end_barriers[end_barrier_count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	end_barriers[end_barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	end_barriers[end_barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	end_barrier_count++;
	command_list->ResourceBarrier(end_barrier_count, end_barriers);

	ID3D12DescriptorHeap *heaps[] = { descriptor_heap };
	command_list->SetDescriptorHeaps(1, heaps);
	command_list->SetComputeRootSignature(root_signature);
	command_list->SetPipelineState(pipeline);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
	command_list->SetComputeRootDescriptorTable(0, gpu_handle);
	gpu_handle.ptr += descriptor_size * 2;
	command_list->SetComputeRootDescriptorTable(1, gpu_handle);
	command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

	D3D12_RESOURCE_BARRIER rgba_barrier = {};
	rgba_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	rgba_barrier.Transition.pResource = dst_rgba;
	rgba_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	rgba_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	rgba_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	command_list->ResourceBarrier(1, &rgba_barrier);

	if (FAILED(command_list->Close())) return false;
	ID3D12CommandList *command_lists[] = { command_list };
	queue->ExecuteCommandLists(1, command_lists);
	d3d12_copy_fence_value++;
	if (FAILED(queue->Signal(fence, d3d12_copy_fence_value))) return false;
	// TODO: Replace this blocking wait with fence-tracked ring textures once the D3D12 path can be verified on Windows.
	if (fence->GetCompletedValue() < d3d12_copy_fence_value) {
		if (FAILED(fence->SetEventOnCompletion(d3d12_copy_fence_value, (HANDLE)d3d12_copy_event))) return false;
		WaitForSingleObject((HANDLE)d3d12_copy_event, INFINITE);
	}

	d3d12_textures_have_rendered = true;
	return true;
#else
	return false;
#endif
}

bool FFmpegPlayer::init_d3d12_context(AVCodecContext *ctx) {
#ifdef FFMPEGGD_HAS_D3D12VA
	using_godot_d3d12_device = false;
	if (hw_device_ctx) {
		av_buffer_unref(&hw_device_ctx);
	}
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->get_rendering_device();
		}
	}
	if (!rd) {
		UtilityFunctions::print("RenderingDevice is unavailable while initializing FFmpeg D3D12VA.");
		return false;
	}

	ID3D12Device *d3d12_device = (ID3D12Device *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	if (!d3d12_device) {
		UtilityFunctions::print("Failed to get D3D12 device from Godot. Is the renderer D3D12?");
		return false;
	}

	hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
	if (!hw_device_ctx) {
		UtilityFunctions::print("Failed to allocate FFmpeg D3D12 device context.");
		return false;
	}

	AVHWDeviceContext *device_ctx = (AVHWDeviceContext *)hw_device_ctx->data;
	AVD3D12VADeviceContext *d3d12_ctx = (AVD3D12VADeviceContext *)device_ctx->hwctx;
	d3d12_device->AddRef();
	d3d12_ctx->device = d3d12_device;

	const int ret = av_hwdevice_ctx_init(hw_device_ctx);
	if (ret < 0) {
		UtilityFunctions::print("Failed to initialize FFmpeg D3D12 context from Godot device: ", ret);
		av_buffer_unref(&hw_device_ctx);
		return false;
	}

	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
	using_godot_d3d12_device = true;
	if (debug_logging) {
		UtilityFunctions::print("Using Godot D3D12 device for FFmpeg D3D12VA.");
	}
	return true;
#else
	return false;
#endif
}

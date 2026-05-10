#include "ffmpeg_player.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <cstring>

void FFmpegPlayer::upload_cpu_frame(const DecodedFrameData &decoded_frame) {
	if (!rd || !decoded_frame.valid) return;

	const int width = decoded_frame.width;
	const int height = decoded_frame.height;
	is_planar_texture = true;

	if (!texture_y_rid.is_valid() || texture_y->get_width() != width || texture_y->get_height() != height || !texture_v_rid.is_valid()) {
		release_textures();

		Ref<RDTextureFormat> fmt_y;
		fmt_y.instantiate();
		fmt_y->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		fmt_y->set_width(width);
		fmt_y->set_height(height);
		fmt_y->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_y->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

		Ref<RDTextureView> view;
		view.instantiate();

		upload_y.resize(width * height);
		upload_u.resize(width * height / 4);
		upload_v.resize(width * height / 4);

		TypedArray<PackedByteArray> data_vec_y; data_vec_y.push_back(upload_y);
		texture_y_rid = rd->texture_create(fmt_y, view, data_vec_y);
		texture_y->set_texture_rd_rid(texture_y_rid);

		Ref<RDTextureFormat> fmt_uv;
		fmt_uv.instantiate();
		fmt_uv->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		fmt_uv->set_width(width / 2);
		fmt_uv->set_height(height / 2);
		fmt_uv->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_uv->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

		TypedArray<PackedByteArray> data_vec_uv; data_vec_uv.push_back(upload_u);
		texture_uv_rid = rd->texture_create(fmt_uv, view, data_vec_uv);
		texture_uv->set_texture_rd_rid(texture_uv_rid);

		Ref<RDTextureFormat> fmt_v;
		fmt_v.instantiate();
		fmt_v->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		fmt_v->set_width(width / 2);
		fmt_v->set_height(height / 2);
		fmt_v->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_v->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

		TypedArray<PackedByteArray> data_vec_v; data_vec_v.push_back(upload_v);
		texture_v_rid = rd->texture_create(fmt_v, view, data_vec_v);
		texture_v->set_texture_rd_rid(texture_v_rid);
	}

	if (upload_y.size() != width * height) upload_y.resize(width * height);
	if (upload_u.size() != width * height / 4) upload_u.resize(width * height / 4);
	if (upload_v.size() != width * height / 4) upload_v.resize(width * height / 4);

	memcpy(upload_y.ptrw(), decoded_frame.y.data(), decoded_frame.y.size());
	memcpy(upload_u.ptrw(), decoded_frame.u.data(), decoded_frame.u.size());
	memcpy(upload_v.ptrw(), decoded_frame.v.data(), decoded_frame.v.size());

	rd->texture_update(texture_y_rid, 0, upload_y);
	rd->texture_update(texture_uv_rid, 0, upload_u);
	rd->texture_update(texture_v_rid, 0, upload_v);
}

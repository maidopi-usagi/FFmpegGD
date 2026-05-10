shader_type canvas_item;

// Y 通道 (Luminance)
uniform sampler2D texture_y : filter_linear, repeat_disable, hint_default_black;
// UV 通道 (Chrominance). 对于 NV12/NV21 这是 RG 纹理，对于 YUV420P 这是 U 分量
uniform sampler2D texture_uv : filter_linear, repeat_disable, hint_default_black;
// V 通道 (仅用于 YUV420P)
uniform sampler2D texture_v : filter_linear, repeat_disable, hint_default_black;

// 格式设置
uniform bool is_yuv420p = false; // true=YUV420P(三平面), false=NV12/NV21(双平面)
uniform bool is_nv21 = false;    // true=VU顺序, false=UV顺序 (仅在 !is_yuv420p 时有效)
uniform bool limited_range = true; // true=视频标准(16-235), false=PC标准(0-255)
uniform int yuv_matrix = 6; // FFmpeg AVColorSpace: 1=BT.709, 6=SMPTE170M/BT.601, 9=BT.2020

vec3 yuv_to_rgb(float y, float u, float v) {
	float r_v;
	float g_u;
	float g_v;
	float b_u;

	if (yuv_matrix == 1) {
		// BT.709
		r_v = 1.5748;
		g_u = -0.187324;
		g_v = -0.468124;
		b_u = 1.8556;
	} else if (yuv_matrix == 9) {
		// BT.2020 non-constant luminance
		r_v = 1.4746;
		g_u = -0.164553;
		g_v = -0.571353;
		b_u = 1.8814;
	} else {
		// BT.601 / SMPTE 170M
		r_v = 1.402;
		g_u = -0.344136;
		g_v = -0.714136;
		b_u = 1.772;
	}

	float yy = limited_range ? 1.16438356 * (y - 0.0625) : y;
	return vec3(yy + r_v * v, yy + g_u * u + g_v * v, yy + b_u * u);
}

void fragment() {
	float y = texture(texture_y, UV).r;
	float u, v;
	
	if (is_yuv420p) {
		u = texture(texture_uv, UV).r - 0.5;
		v = texture(texture_v, UV).r - 0.5;
	} else {
		vec2 uv_sample = texture(texture_uv, UV).rg;
		
		if (is_nv21) {
			u = uv_sample.y - 0.5; // V is first in NV21? No, usually NV21 is Y, V, U. 
			                       // Textures: NV12 is UVUV, NV21 is VUVU.
			v = uv_sample.x - 0.5;
		} else {
			u = uv_sample.x - 0.5;
			v = uv_sample.y - 0.5;
		}
	}
	
	vec3 rgb = yuv_to_rgb(y, u, v);
	COLOR = vec4(clamp(rgb, vec3(0.0), vec3(1.0)), 1.0);
}

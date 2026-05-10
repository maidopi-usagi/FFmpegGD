shader_type spatial;
render_mode unshaded, cull_disabled;

uniform sampler2D texture_y : filter_linear, repeat_disable, hint_default_black;
uniform sampler2D texture_uv : filter_linear, repeat_disable, hint_default_black;
uniform sampler2D texture_v : filter_linear, repeat_disable, hint_default_black;

uniform bool is_yuv420p = false;
uniform bool limited_range = true;
uniform int yuv_matrix = 6;
uniform int color_transfer = 2;
uniform float hdr_source_peak_nits = 0.0;

vec3 yuv_to_rgb(float y, float u, float v) {
	float r_v;
	float g_u;
	float g_v;
	float b_u;

	if (yuv_matrix == 1) {
		r_v = 1.5748;
		g_u = -0.187324;
		g_v = -0.468124;
		b_u = 1.8556;
	} else if (yuv_matrix == 9) {
		r_v = 1.4746;
		g_u = -0.164553;
		g_v = -0.571353;
		b_u = 1.8814;
	} else {
		r_v = 1.402;
		g_u = -0.344136;
		g_v = -0.714136;
		b_u = 1.772;
	}

	float yy = limited_range ? 1.16438356 * (y - 0.0625) : y;
	return vec3(yy + r_v * v, yy + g_u * u + g_v * v, yy + b_u * u);
}

vec3 pq_to_linear(vec3 value) {
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 p = pow(max(value, vec3(0.0)), vec3(1.0 / m2));
	return pow(max(p - vec3(c1), vec3(0.0)) / max(vec3(c2) - vec3(c3) * p, vec3(0.000001)), vec3(1.0 / m1));
}

vec3 hlg_to_linear(vec3 value) {
	const float a = 0.17883277;
	const float b = 0.28466892;
	const float c = 0.55991073;
	vec3 low = (value * value) / 3.0;
	vec3 high = (exp((value - vec3(c)) / a) + vec3(b)) / 12.0;
	return mix(low, high, step(vec3(0.5), value));
}

vec3 map_hdr_nits(vec3 linear) {
	const float target_peak_nits = 100.0;
	float peak = max(hdr_source_peak_nits, target_peak_nits);
	float shoulder = peak / target_peak_nits;
	vec3 nits = linear * 10000.0;
	vec3 x = nits / target_peak_nits;
	vec3 mapped = x * (1.0 + x / (shoulder * shoulder)) / (1.0 + x);
	return pow(clamp(mapped, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
}

vec3 tone_map(vec3 rgb) {
	if (color_transfer == 16) {
		return map_hdr_nits(pq_to_linear(clamp(rgb, vec3(0.0), vec3(1.0))));
	}
	if (color_transfer == 18) {
		return map_hdr_nits(hlg_to_linear(clamp(rgb, vec3(0.0), vec3(1.0))));
	}
	return rgb;
}

void fragment() {
	float y = texture(texture_y, UV).r;
	float u;
	float v;
	if (is_yuv420p) {
		u = texture(texture_uv, UV).r - 0.5;
		v = texture(texture_v, UV).r - 0.5;
	} else {
		vec2 uv_sample = texture(texture_uv, UV).rg;
		u = uv_sample.x - 0.5;
		v = uv_sample.y - 0.5;
	}
	ALBEDO = clamp(tone_map(yuv_to_rgb(y, u, v)), vec3(0.0), vec3(1.0));
	ALPHA = 1.0;
}

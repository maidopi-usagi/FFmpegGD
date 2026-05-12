shader_type canvas_item;

uniform samplerExternalOES video_texture;

void fragment() {
	COLOR = texture(video_texture, UV);
}

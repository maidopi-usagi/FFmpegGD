extends Node3D

var player: FFmpegPlayer
var screen: MeshInstance3D
var rgba_material: StandardMaterial3D
var yuv_material: ShaderMaterial
var file_dialog: FileDialog
var status_label: Label
var stats_label: Label
var open_button: Button
var rotate_check: CheckBox
var current_file_path := ""
var media_loaded := false

func _ready():
	player = FFmpegPlayer.new()
	add_child(player)

	_create_world()
	_create_ui()
	get_window().files_dropped.connect(_on_files_dropped)

func _process(delta):
	_update_video_material()
	_update_stats()
	if rotate_check and rotate_check.button_pressed:
		screen.rotation_degrees.y += delta * 18.0

func _create_world():
	var camera = Camera3D.new()
	camera.position = Vector3(0.0, 1.2, 4.0)
	add_child(camera)
	camera.look_at(Vector3.ZERO)

	var light = DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45.0, 35.0, 0.0)
	add_child(light)

	screen = MeshInstance3D.new()
	var quad = QuadMesh.new()
	quad.size = Vector2(3.2, 1.8)
	screen.mesh = quad
	screen.position = Vector3(0.0, 0.6, 0.0)
	add_child(screen)

	rgba_material = StandardMaterial3D.new()
	rgba_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	rgba_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	screen.material_override = rgba_material

	yuv_material = ShaderMaterial.new()
	yuv_material.shader = load("res://video_3d_yuv.gdshader")

	var floor_mesh = MeshInstance3D.new()
	var plane = PlaneMesh.new()
	plane.size = Vector2(6.0, 6.0)
	floor_mesh.mesh = plane
	floor_mesh.position = Vector3(0.0, -0.45, 0.0)
	var floor_material = StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.12, 0.13, 0.15)
	floor_mesh.material_override = floor_material
	add_child(floor_mesh)

func _create_ui():
	var canvas = CanvasLayer.new()
	add_child(canvas)

	var controls = PanelContainer.new()
	controls.set_anchors_preset(Control.PRESET_TOP_WIDE)
	controls.offset_left = 16
	controls.offset_right = -16
	controls.offset_top = 16
	controls.offset_bottom = 100
	canvas.add_child(controls)

	var column = VBoxContainer.new()
	column.add_theme_constant_override("separation", 6)
	controls.add_child(column)

	var row = HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	column.add_child(row)

	open_button = Button.new()
	open_button.text = "Open Video"
	open_button.pressed.connect(_on_open_pressed)
	row.add_child(open_button)

	rotate_check = CheckBox.new()
	rotate_check.text = "Rotate screen"
	rotate_check.button_pressed = true
	row.add_child(rotate_check)

	status_label = Label.new()
	status_label.text = "Drop a video file here or use Open Video"
	status_label.clip_text = true
	status_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(status_label)

	stats_label = Label.new()
	stats_label.text = ""
	stats_label.clip_text = true
	column.add_child(stats_label)

	file_dialog = FileDialog.new()
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	file_dialog.title = "Open video file"
	file_dialog.file_selected.connect(_load_media)
	canvas.add_child(file_dialog)

func _on_open_pressed():
	file_dialog.popup_centered_ratio(0.8)

func _on_files_dropped(files: PackedStringArray):
	if files.is_empty():
		return
	_load_media(files[0])

func _load_media(path: String):
	current_file_path = path
	player.stop()
	media_loaded = true
	status_label.text = "Loading: " + path.get_file()
	player.load_file(path)
	player.play()
	if not player.is_playing():
		media_loaded = false
		status_label.text = "Failed: " + path.get_file() + " " + player.get_last_error_message()
		return
	status_label.text = "Playing on 3D mesh: " + path.get_file()

func _update_video_material():
	var tex_external = player.get_video_texture_external()
	if tex_external:
		if screen.material_override != rgba_material:
			screen.material_override = rgba_material
		rgba_material.albedo_texture = tex_external
		return

	var tex_rgba = player.get_video_texture_rgba()
	if tex_rgba:
		if screen.material_override != rgba_material:
			screen.material_override = rgba_material
		rgba_material.albedo_texture = tex_rgba
		return

	var tex_y = player.get_video_texture_y()
	var tex_uv = player.get_video_texture_uv()
	if tex_y and tex_uv:
		if screen.material_override != yuv_material:
			screen.material_override = yuv_material
		yuv_material.set_shader_parameter("texture_y", tex_y)
		yuv_material.set_shader_parameter("texture_uv", tex_uv)
		yuv_material.set_shader_parameter("is_yuv420p", player.is_yuv420p())
		yuv_material.set_shader_parameter("limited_range", player.get_video_color_range() != 2)
		yuv_material.set_shader_parameter("yuv_matrix", player.get_video_colorspace())
		yuv_material.set_shader_parameter("color_transfer", player.get_video_color_transfer())
		yuv_material.set_shader_parameter("hdr_source_peak_nits", player.get_video_hdr_source_peak_nits())
		var tex_v = player.get_video_texture_v()
		if player.is_yuv420p() and tex_v:
			yuv_material.set_shader_parameter("texture_v", tex_v)

func _update_stats():
	if not media_loaded:
		stats_label.text = ""
		return
	var size = player.get_video_size()
	var resolution = "%dx%d" % [size.x, size.y] if size.x > 0 and size.y > 0 else "unknown"
	var backend = player.get_video_decoder_backend_name()
	if backend.is_empty():
		backend = "unknown"
	var path = "ExternalTexture" if player.get_video_texture_external() else ("GPU RGBA" if player.get_video_texture_rgba() else "YUV spatial shader")
	stats_label.text = "3D video texture | %s | %s | %s | %.1f fps / %.3f ms" % [resolution, backend, path, player.get_last_video_fps(), player.get_last_upload_ms()]

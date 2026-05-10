extends Control

var player: FFmpegPlayer
var texture_rect: TextureRect
var shader_material: ShaderMaterial
var controls: PanelContainer
var drop_label: Label
var file_dialog: FileDialog
var file_button: Button
var play_button: Button
var progress_slider: HSlider
var time_label: Label
var duration_label: Label
var status_label: Label
var debug_check: CheckBox
var audio_player: AudioStreamPlayer
var audio_playback: AudioStreamGeneratorPlayback
var audio_prebuffer_frames := 0
var last_audio_skips := 0
var is_dragging := false
var was_playing_before_drag := false
var last_preview_seek_msec := 0
var media_loaded := false
var current_file_path := ""
const PREVIEW_SEEK_INTERVAL_MSEC := 120

func _ready():
	player = FFmpegPlayer.new()
	add_child(player)
	
	texture_rect = TextureRect.new()
	texture_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	texture_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	texture_rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	add_child(texture_rect)
	_create_drop_label()
	_create_controls()
	_create_file_dialog()
	
	shader_material = ShaderMaterial.new()
	shader_material.shader = load("res://nv12_to_rgb.gdshader")
	texture_rect.material = shader_material
	get_window().files_dropped.connect(_on_files_dropped)

func _process(delta):
	_update_controls()

	_update_audio_output()
	_update_video_texture()

func _create_audio_output():
	_stop_audio_output()
	if audio_player:
		audio_player.queue_free()
		audio_player = null
	if not player.has_audio():
		return
	var stream = AudioStreamGenerator.new()
	stream.mix_rate_mode = AudioStreamGenerator.MIX_RATE_CUSTOM
	stream.mix_rate = player.get_audio_mix_rate()
	stream.buffer_length = 1.0
	audio_prebuffer_frames = int(stream.mix_rate * 0.5)
	audio_player = AudioStreamPlayer.new()
	audio_player.stream = stream
	add_child(audio_player)

func _create_drop_label():
	drop_label = Label.new()
	drop_label.set_anchors_preset(Control.PRESET_FULL_RECT)
	drop_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	drop_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	drop_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	drop_label.text = "Drop a video or audio file here to play"
	drop_label.add_theme_font_size_override("font_size", 24)
	drop_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(drop_label)

func _create_file_dialog():
	file_dialog = FileDialog.new()
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	file_dialog.title = "Open media file"
	file_dialog.file_selected.connect(_load_media)
	add_child(file_dialog)

func _create_controls():
	controls = PanelContainer.new()
	controls.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	controls.offset_left = 16
	controls.offset_right = -16
	controls.offset_top = -96
	controls.offset_bottom = -12
	add_child(controls)

	var column = VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	controls.add_child(column)

	progress_slider = HSlider.new()
	progress_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	progress_slider.min_value = 0.0
	progress_slider.max_value = 1.0
	progress_slider.step = 0.01
	progress_slider.drag_started.connect(_on_slider_drag_started)
	progress_slider.drag_ended.connect(_on_slider_drag_ended)
	progress_slider.value_changed.connect(_on_slider_value_changed)
	column.add_child(progress_slider)

	var row = HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	column.add_child(row)

	file_button = Button.new()
	file_button.text = "Open"
	file_button.custom_minimum_size = Vector2(80, 0)
	file_button.pressed.connect(_on_open_button_pressed)
	row.add_child(file_button)

	play_button = Button.new()
	play_button.text = "Play"
	play_button.custom_minimum_size = Vector2(80, 0)
	play_button.pressed.connect(_on_play_button_pressed)
	row.add_child(play_button)

	time_label = Label.new()
	time_label.text = "00:00"
	time_label.custom_minimum_size = Vector2(56, 0)
	row.add_child(time_label)

	duration_label = Label.new()
	duration_label.text = "00:00"
	duration_label.custom_minimum_size = Vector2(56, 0)
	row.add_child(duration_label)

	debug_check = CheckBox.new()
	debug_check.text = "Debug"
	debug_check.toggled.connect(_on_debug_toggled)
	row.add_child(debug_check)

	status_label = Label.new()
	status_label.text = "No file loaded"
	status_label.clip_text = true
	status_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(status_label)

func _update_controls():
	var duration = player.get_duration()
	var position = player.get_position()
	play_button.text = "Pause" if player.is_playing() else "Play"
	play_button.disabled = not media_loaded
	progress_slider.editable = media_loaded and duration > 0.0
	time_label.text = _format_time(position)
	duration_label.text = _format_time(duration)
	if duration > 0.0:
		progress_slider.max_value = duration
		if not is_dragging:
			progress_slider.value = clamp(position, 0.0, duration)

func _on_play_button_pressed():
	if not media_loaded:
		return
	if player.is_playing():
		player.stop()
		_stop_audio_output()
	else:
		_stop_audio_output()
		player.play()

func _on_open_button_pressed():
	file_dialog.popup_centered_ratio(0.8)

func _on_debug_toggled(enabled: bool):
	player.set_debug_logging(enabled)

func _on_slider_drag_started():
	if not media_loaded:
		return
	was_playing_before_drag = player.is_playing()
	if was_playing_before_drag:
		player.stop()
	_stop_audio_output()
	last_preview_seek_msec = 0
	is_dragging = true

func _on_slider_drag_ended(value_changed: bool):
	is_dragging = false
	if value_changed:
		player.seek(progress_slider.value)
		_stop_audio_output()
	if was_playing_before_drag:
		player.play()
	else:
		player.stop()

func _on_slider_value_changed(value: float):
	if is_dragging and media_loaded:
		time_label.text = _format_time(value)
		var now = Time.get_ticks_msec()
		if now - last_preview_seek_msec >= PREVIEW_SEEK_INTERVAL_MSEC:
			last_preview_seek_msec = now
			player.seek(value)
			_stop_audio_output()
			player.play()

func _update_audio_output():
	if audio_player == null or is_dragging or not player.is_playing():
		return
	if audio_playback == null:
		if player.get_queued_audio_frame_count() < audio_prebuffer_frames:
			return
		audio_player.play()
		audio_playback = audio_player.get_stream_playback()
		last_audio_skips = 0
	if audio_playback == null:
		return
	var available = min(audio_playback.get_frames_available(), int(player.get_audio_mix_rate() / 10))
	if available <= 0:
		return
	var frames = player.pop_audio_frames(available)
	if frames.size() > 0:
		audio_playback.push_buffer(frames)
	var skips = audio_playback.get_skips()
	if debug_check.button_pressed and skips != last_audio_skips:
		print("Audio generator skips: ", skips)
		last_audio_skips = skips

func _stop_audio_output():
	if audio_player:
		audio_player.stop()
	audio_playback = null

func _on_files_dropped(files: PackedStringArray):
	if files.is_empty():
		return
	_load_media(files[0])

func _load_media(path: String):
	current_file_path = path
	_stop_audio_output()
	player.stop()
	texture_rect.texture = null
	texture_rect.material = shader_material
	media_loaded = true
	is_dragging = false
	progress_slider.value = 0.0
	progress_slider.max_value = 1.0
	time_label.text = "00:00"
	duration_label.text = "00:00"
	if drop_label:
		drop_label.text = "Loading:\n" + path
		drop_label.visible = true
	status_label.text = "Loading: " + path.get_file()
	player.load_file(path)
	_create_audio_output()
	player.play()
	if not player.is_playing():
		media_loaded = false
		status_label.text = "Failed to load: " + path.get_file()
		if drop_label:
			drop_label.text = "Could not play this file\nDrop another file or use Open"
			drop_label.visible = true
		return
	if drop_label:
		drop_label.visible = false
	status_label.text = "Playing: " + path.get_file()

func _update_video_texture():
	var tex_rgba = player.get_video_texture_rgba()
	if tex_rgba:
		if drop_label:
			drop_label.visible = false
		if texture_rect.material != null:
			texture_rect.material = null
		if texture_rect.texture != tex_rgba:
			texture_rect.texture = tex_rgba
		return

	var tex_y = player.get_video_texture_y()
	var tex_uv = player.get_video_texture_uv()
	var tex_v = player.get_video_texture_v()
	var is_planar = player.is_yuv420p()
	if tex_y and tex_uv:
		if drop_label:
			drop_label.visible = false
		if texture_rect.material != shader_material:
			texture_rect.material = shader_material
		if texture_rect.texture != tex_y:
			texture_rect.texture = tex_y
		shader_material.set_shader_parameter("texture_y", tex_y)
		shader_material.set_shader_parameter("texture_uv", tex_uv)
		shader_material.set_shader_parameter("is_yuv420p", is_planar)
		shader_material.set_shader_parameter("is_nv21", false)
		shader_material.set_shader_parameter("limited_range", player.get_video_color_range() != 2)
		shader_material.set_shader_parameter("yuv_matrix", player.get_video_colorspace())
		if is_planar and tex_v:
			shader_material.set_shader_parameter("texture_v", tex_v)

func _format_time(seconds: float) -> String:
	if seconds < 0.0 or is_nan(seconds) or is_inf(seconds):
		seconds = 0.0
	var total = int(seconds)
	var minutes = int(total / 60)
	var secs = total % 60
	return "%02d:%02d" % [minutes, secs]

@tool
extends EditorPlugin

var export_plugin: EditorExportPlugin

func _enter_tree():
	export_plugin = FFmpegGDAndroidExportPlugin.new()
	add_export_plugin(export_plugin)

func _exit_tree():
	if export_plugin:
		remove_export_plugin(export_plugin)
		export_plugin = null

class FFmpegGDAndroidExportPlugin extends EditorExportPlugin:
	func _get_name() -> String:
		return "FFmpegGDAndroid"

	func _supports_platform(platform: EditorExportPlatform) -> bool:
		return platform.get_os_name() == "Android"

	func _get_android_libraries(platform: EditorExportPlatform, debug: bool) -> PackedStringArray:
		return PackedStringArray(["res://addons/ffmpeg_gd_android/ffmpeg_gd_android.aar"])

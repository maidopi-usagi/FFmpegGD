package org.godotengine.plugin.android.ffmpeggd;

import org.godotengine.godot.Godot;
import org.godotengine.godot.plugin.GodotPlugin;

import java.util.Collections;
import java.util.Set;

public class FFmpegGDPlugin extends GodotPlugin {
    static {
        System.loadLibrary("ffmpeg");
        System.loadLibrary("ffmpeg_gd.android.template_debug.arm64");
    }

    public FFmpegGDPlugin(Godot godot) {
        super(godot);
    }

    @Override
    public String getPluginName() {
        return "FFmpegGDPlugin";
    }

    @Override
    public Set<String> getPluginGDExtensionLibrariesPaths() {
        return Collections.singleton("res://addons/ffmpeg_gd_android/ffmpeg_gd.gdextension");
    }
}

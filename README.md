# ffmpeg_gd

Godot GDExtension video player backed by FFmpeg, aims at high effciency hardware replay support for available plaforms without CPU rounding.

## Platform Status

| Platform | Backend | Status |
| --- | --- | --- |
| macOS arm64 | VideoToolbox + Metal | Supported. GPU decode with no CPU pixel readback; NV12->RGBA conversion runs on Metal. |
| Windows x86_64 | D3D12VA | D3D12 supported. Vulkan lacks external memory suppport. |
| Android arm64 | MediaCodec + Vulkan copy or GLES ExternalTexture | Supported through the Godot Android plugin path. Vulkan can hardware-decode but needs a copy/upload path; GLES Compatibility can use `SurfaceTexture` + `ExternalTexture` for the zero-copy display path. |
| Linux | TBD | Not shipped yet. |
| iOS / Web | TBD | Not shipped yet. |

## Build

Use a system or user-provided FFmpeg development install. The build does not download FFmpeg automatically because FFmpeg's effective license depends on the way that FFmpeg binary was configured.

```shell
scons platform=macos target=template_debug arch=arm64 ffmpeg_path="/opt/homebrew/opt/ffmpeg"
```

If `ffmpeg_path` is omitted, SCons tries `pkg-config`:

```shell
scons platform=linux target=template_debug
```

`ffmpeg_path` must point at a directory containing `include/` and `lib/`.

For CMake, use the equivalent `FFMPEG_ROOT` cache variable or omit it to use `pkg-config`:

```shell
cmake -S . -B cmake-build -DFFMPEG_ROOT=/opt/homebrew/opt/ffmpeg
```

On Windows, FFmpeg runtime DLLs are not copied by default. To explicitly bundle them for a local demo/package:

```shell
scons platform=windows target=template_debug ffmpeg_path="C:/path/to/ffmpeg" ffmpeg_bundle_runtime=yes
```

Only enable runtime bundling after verifying the selected FFmpeg build's license and redistribution requirements.

The extension config lives at `demo/bin/ffmpeg_gd.gdextension` and uses the `ffmpeg_gd_library_init` entry symbol.

### Android

Android debug exports use `demo/export_presets.cfg`, Godot's Gradle export, and the plugin in `demo/addons/ffmpeg_gd_android`. The plugin AAR loads `libffmpeg.so` and `libffmpeg_gd.android.template_debug.arm64.so` from Java so `JNI_OnLoad` can register the Java VM with FFmpeg before the GDExtension initializes MediaCodec.

The AAR is prebuilt in this repository. You do not need to rebuild the plugin with Gradle for a normal export; rebuild or repack the AAR only after changing Java code, AAR assets, or bundled native libraries. Godot's Android export still needs Gradle because Android Plugin v2 AARs are packaged by Gradle export; pure non-Gradle APK export is not the documented path for v2 plugins.

For Android rendering, Vulkan/Forward+ can still use MediaCodec hardware decode, but stock Godot 4.6 cannot expose the decoded image as a Vulkan `ExternalTexture` yet, so that route needs a copy/upload path. GLES Compatibility returns a valid `ExternalTexture` ID for `android.graphics.SurfaceTexture`, which enables the pure hardware display path.

Typical debug build/deploy flow:

```shell
ANDROID_HOME= ANDROID_SDK_ROOT= ANDROID_NDK_ROOT="/Users/opi/Library/Android/sdk/ndk/28.1.13356709" \
  scons platform=android target=template_debug arch=arm64 android_api_level=26 \
  ffmpeg_path="/path/to/android/ffmpeg-root"

cp demo/bin/android/libffmpeg_gd.android.template_debug.arm64.so \
  android_plugin/build/aar/jni/arm64-v8a/libffmpeg_gd.android.template_debug.arm64.so

(cd android_plugin/build/aar && rm -f ../../../demo/addons/ffmpeg_gd_android/ffmpeg_gd_android.aar && \
  zip -r ../../../demo/addons/ffmpeg_gd_android/ffmpeg_gd_android.aar .)

godot --headless --path demo --export-debug Android build/android/ffmpeg_gd-debug.apk
adb install -r demo/build/android/ffmpeg_gd-debug.apk
```

The export destination is relative to the Godot project path, so `build/android/ffmpeg_gd-debug.apk` is written under `demo/build/android/` from the repository root.

The demo requests shared-storage media permissions and auto-opens `/storage/emulated/0/Pictures/15-33-14.mp4` on Android for device testing.

## Demo

Open the `demo` project in Godot and run the main scene.

The demo supports drag-and-drop playback and an `Open` button. Enable the `Debug` checkbox to print FFmpeg/player diagnostics.

On Android, the GLES external texture path is drawn with `demo/external_texture.gdshader`, which uses `samplerExternalOES`. Enable the Debug checkbox when you need verbose decoder/backend logs such as `Using hardware acceleration: mediacodec`.

## License

This project's own source code is licensed under the MIT License. See `LICENSE.md`.

## FFmpeg License Notes

This project links to FFmpeg libraries but does not vendor FFmpeg binaries. The MIT License covers this project only; FFmpeg remains under the license terms of the exact FFmpeg build you use. For the lowest redistribution friction, use dynamically linked LGPL FFmpeg builds and avoid GPL/nonfree FFmpeg configurations unless you are prepared to satisfy those license terms for your distribution.

When distributing a package that includes FFmpeg shared libraries, include the applicable FFmpeg license notices and source/offer requirements for the exact FFmpeg build you ship. This is a project policy note, not legal advice.

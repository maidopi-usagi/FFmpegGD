# ffmpeg_gd

Godot FFmpeg GDExtension video player. Focus: hardware decode and low-copy display where platform allows it.


## Status

| Platform | Backend | Status | 0 copy
| --- | --- | --- | --- |
| Windows x64 D3D12 | D3D12VA | Builds. | Yes |
| Windows x64 Vulkan| VkVideoCodec | Builds. | Yes(Hooked) |
| macOS arm64 | VideoToolbox + Metal | Builds. | Yes |
| iOS arm64 | VideoToolbox + Metal | Builds. | Yes |
| Android arm64 GLES| MediaCodec | Builds. | Yes |
| Android arm64 Vulkan| MediaCodec | Builds. | HW Decode but needs copy planes. |
| Linux | Vulkan | Builds, lightly tested. |
| Web | - | Not supported. |

## Common

Use SCons for normal builds. Provide FFmpeg root with `include/` and `lib/`.

```shell
scons platform=macos target=template_debug arch=arm64 ffmpeg_path="/path/to/ffmpeg"
```

If `ffmpeg_path` is omitted, SCons tries `pkg-config`.

```shell
scons platform=linux target=template_debug
```

GDExtension config: `demo/bin/ffmpeg_gd.gdextension`.

## macOS

```shell
scons platform=macos target=template_debug arch=arm64 ffmpeg_path="/opt/homebrew/opt/ffmpeg"
```

Output:

```text
demo/bin/macos/libffmpeg_gd.macos.template_debug.arm64.dylib
```

## iOS

Prepare iOS FFmpeg root first.

```shell
scons platform=ios target=template_debug arch=arm64 ffmpeg_path="/path/to/ffmpeg-ios-root"
```

Create XCFramework used by Godot export:

```shell
rm -rf demo/bin/ios/libffmpeg_gd.ios.template_debug.xcframework
xcodebuild -create-xcframework \
  -library demo/bin/ios/libffmpeg_gd.ios.template_debug.arm64.dylib \
  -output demo/bin/ios/libffmpeg_gd.ios.template_debug.xcframework
```

Optional: generate Xcode project from Godot preset:

```shell
mkdir -p demo/build/ios
/Applications/Godot.app/Contents/MacOS/Godot --headless --path demo --export-debug iOS build/ios/ffmpeg_gd.xcodeproj
```

Optional: compile generated Xcode project:

```shell
xcodebuild -project demo/build/ios/ffmpeg_gd.xcodeproj \
  -scheme ffmpeg_gd \
  -configuration Debug \
  -destination 'generic/platform=iOS' \
  build
```

## Android

Arm64 only. Uses Godot Android Plugin v2 AAR.

```shell
ANDROID_HOME= ANDROID_SDK_ROOT= ANDROID_NDK_ROOT="/path/to/android-ndk" \
  scons platform=android target=template_debug arch=arm64 android_api_level=26 \
  ffmpeg_path="/path/to/android/ffmpeg-root"
```

Repack AAR after native rebuild:

```shell
cp demo/bin/android/libffmpeg_gd.android.template_debug.arm64.so \
  android_plugin/build/aar/jni/arm64-v8a/libffmpeg_gd.android.template_debug.arm64.so

(cd android_plugin/build/aar && rm -f ../../../demo/addons/ffmpeg_gd_android/ffmpeg_gd_android.aar && \
  zip -r ../../../demo/addons/ffmpeg_gd_android/ffmpeg_gd_android.aar .)
```

## Windows

```shell
scons platform=windows target=template_debug arch=x86_64 ffmpeg_path="C:/path/to/ffmpeg"
```

Bundle FFmpeg DLLs into demo output:

```shell
scons platform=windows target=template_debug arch=x86_64 ffmpeg_path="C:/path/to/ffmpeg" ffmpeg_bundle_runtime=yes
```

## Linux

```shell
scons platform=linux target=template_debug arch=x86_64
# or
scons platform=linux target=template_debug arch=x86_64 ffmpeg_path="/path/to/ffmpeg"
```

## CMake

Desktop fallback path. SCons is preferred.

```shell
cmake -S . -B cmake-build -DFFMPEG_ROOT="/path/to/ffmpeg"
cmake --build cmake-build
```

## License

This project's own source code is licensed under the MIT License. See `LICENSE.md`.

## FFmpeg License Notes

This project links to FFmpeg libraries but does not vendor FFmpeg binaries. The MIT License covers this project only; FFmpeg remains under the license terms of the exact FFmpeg build you use. For the lowest redistribution friction, use dynamically linked LGPL FFmpeg builds and avoid GPL/nonfree FFmpeg configurations unless you are prepared to satisfy those license terms for your distribution.

When distributing a package that includes FFmpeg shared libraries, include the applicable FFmpeg license notices and source/offer requirements for the exact FFmpeg build you ship. This is a project policy note, not legal advice.

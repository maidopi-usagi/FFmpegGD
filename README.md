# ffmpeg_gd

Godot GDExtension video player backed by FFmpeg.

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

## Demo

Open the `demo` project in Godot and run the main scene.

The demo supports drag-and-drop playback and an `Open` button. Enable the `Debug` checkbox to print FFmpeg/player diagnostics.

## License

This project's own source code is licensed under the MIT License. See `LICENSE.md`.

## FFmpeg License Notes

This project links to FFmpeg libraries but does not vendor FFmpeg binaries. The MIT License covers this project only; FFmpeg remains under the license terms of the exact FFmpeg build you use. For the lowest redistribution friction, use dynamically linked LGPL FFmpeg builds and avoid GPL/nonfree FFmpeg configurations unless you are prepared to satisfy those license terms for your distribution.

When distributing a package that includes FFmpeg shared libraries, include the applicable FFmpeg license notices and source/offer requirements for the exact FFmpeg build you ship. This is a project policy note, not legal advice.

#!/usr/bin/env python
import os
import sys
import glob

from methods import print_error


libname = "ffmpeg_gd"
projectdir = "demo"

localEnv = Environment(tools=["default"], PLATFORM="")

# Build profiles can be used to decrease compile times.
# You can either specify "disabled_classes", OR
# explicitly specify "enabled_classes" which disables all other classes.
# Modify the build profile path as needed and uncomment the line below or
# manually specify the build_profile parameter when running SCons.

# localEnv["build_profile"] = "build_profile.json"

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Add(PathVariable("ffmpeg_path", "Path to an FFmpeg installation with include/ and lib/ directories", os.environ.get("FFMPEG_PATH", os.environ.get("FFMPEG_DIR", "")), PathVariable.PathAccept))
opts.Add(BoolVariable("ffmpeg_bundle_runtime", "Copy FFmpeg shared libraries into the demo output. This may add FFmpeg redistribution license obligations.", False))
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

# FFmpeg configuration. Do not auto-download or bundle FFmpeg: the FFmpeg
# license depends on how it was configured. Prefer a system/user-provided LGPL
# shared build, and make redistribution an explicit packaging decision.
ffmpeg_path = env.get("ffmpeg_path", "")
ffmpeg_dlls = []
ffmpeg_uses_pkg_config = False
if ffmpeg_path:
    ffmpeg_path = os.path.abspath(ffmpeg_path)
    ffmpeg_include = os.path.join(ffmpeg_path, "include")
    ffmpeg_lib = os.path.join(ffmpeg_path, "lib")
    if not os.path.isdir(ffmpeg_include) or not os.path.isdir(ffmpeg_lib):
        print_error(f"Invalid ffmpeg_path: {ffmpeg_path}. Expected include/ and lib/ directories.")
        sys.exit(1)
    print(f"Using FFmpeg at: {ffmpeg_path}")
    env.Append(CPPPATH=[ffmpeg_include])
    env.Append(LIBPATH=[ffmpeg_lib])
elif env.Detect("pkg-config"):
    print("Using FFmpeg from pkg-config.")
    env.ParseConfig("pkg-config --cflags --libs libavcodec libavformat libavutil libswscale libswresample")
    ffmpeg_uses_pkg_config = True
else:
    print_error("FFmpeg development libraries not found. Set ffmpeg_path=/path/to/ffmpeg or install pkg-config metadata for FFmpeg.")
    sys.exit(1)

if env["platform"] == "windows" and env.get("ffmpeg_bundle_runtime", False):
    if not ffmpeg_path:
        print_error("ffmpeg_bundle_runtime=yes requires ffmpeg_path so DLLs can be located explicitly.")
        sys.exit(1)
    print("Bundling FFmpeg runtime DLLs. Verify the selected FFmpeg build license before redistribution.")
    ffmpeg_bin = os.path.join(ffmpeg_path, "bin")
    if os.path.exists(ffmpeg_bin):
        dlls = glob.glob(os.path.join(ffmpeg_bin, "*.dll"))
        output_bin = f"demo/bin/{env['platform']}/"
        for dll in dlls:
            ffmpeg_dlls.append(env.Install(output_bin, dll))

# Vulkan configuration
vulkan_sdk = os.environ.get("VULKAN_SDK")
if vulkan_sdk:
    env.Append(CPPPATH=[os.path.join(vulkan_sdk, "Include")])
    env.Append(LIBPATH=[os.path.join(vulkan_sdk, "Lib")])

# Link FFmpeg libraries
# Adjust library names based on your specific FFmpeg build (e.g., might need version suffixes on Linux)
if env["platform"] == "windows":
    if not ffmpeg_uses_pkg_config:
        env.Append(LIBS=['avcodec', 'avformat', 'avutil', 'swscale', 'swresample'])
    env.Append(LIBS=['vulkan-1', 'd3d12', 'd3dcompiler'])
elif env["platform"] == "linux":
    if not ffmpeg_uses_pkg_config:
        env.Append(LIBS=['avcodec', 'avformat', 'avutil', 'swscale', 'swresample'])
    env.Append(LIBS=['vulkan'])
elif env["platform"] == "macos":
    if not ffmpeg_uses_pkg_config:
        env.Append(LIBS=['avcodec', 'avformat', 'avutil', 'swscale', 'swresample'])
    env.Append(LINKFLAGS=['-framework', 'VideoToolbox', '-framework', 'CoreVideo', '-framework', 'CoreMedia', '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'QuartzCore', '-lobjc'])
elif env["platform"] == "ios":
    if not ffmpeg_uses_pkg_config:
        env.Append(LIBS=['avcodec', 'avformat', 'avutil', 'swscale', 'swresample'])
    env.Append(CPPDEFINES=['FFMPEGGD_HAS_IOS_DOCUMENT_PICKER'])
    env.Append(LINKFLAGS=['-framework', 'VideoToolbox', '-framework', 'CoreVideo', '-framework', 'CoreMedia', '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'QuartzCore', '-framework', 'UIKit', '-framework', 'UniformTypeIdentifiers', '-lobjc'])
elif env["platform"] == "android":
    if not ffmpeg_uses_pkg_config:
        env.Append(LIBS=['avcodec', 'avformat', 'avutil', 'swscale', 'swresample'])
    env.Append(LIBS=['android', 'log', 'mediandk'])

env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")
if env["platform"] not in ["linux", "windows"]:
    sources = [source for source in sources if os.path.basename(str(source)) != "ffmpeg_player_vulkan.cpp"]
if env["platform"] == "macos":
    sources += ["src/ffmpeg_player_videotoolbox.mm"]
elif env["platform"] == "ios":
    sources += Glob("src/*.mm")

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# .dev doesn't inhibit compatibility, so we don't need to key it.
# .universal just means "compatible with all relevant arches" so we don't need to key it.
suffix = env['suffix'].replace(".dev", "").replace(".universal", "")

lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), libname, suffix, env.subst('$SHLIBSUFFIX'))

library = env.SharedLibrary(
    "bin/{}/{}".format(env['platform'], lib_filename),
    source=sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

default_args = [library, copy]
if 'ffmpeg_dlls' in locals():
    default_args += ffmpeg_dlls

Default(*default_args)

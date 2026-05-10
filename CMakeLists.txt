# Using the same minimum as the godot-cpp project
cmake_minimum_required(VERSION 3.17)

# Silence unused variable warning when specified from toolchain
if(CMAKE_C_COMPILER)
endif()

set(LIBNAME "ffmpeg_gd" CACHE STRING "The name of the library")
set(GODOT_PROJECT_DIR "demo" CACHE STRING "The directory of a Godot project folder")
set(FFMPEG_ROOT "" CACHE PATH "Path to an FFmpeg installation with include/ and lib/ directories")

# Make sure all the dependencies are satisfied
find_package(Python3 3.4 REQUIRED)
find_program(GIT git REQUIRED)

# Ensure godot-cpp submodule has been updated
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/godot-cpp/src")
    message(NOTICE "godot-cpp bindings source not found")
    message(NOTICE "initializing/updating the godot-cpp submodule...")

    # update the c++ bindings submodule to populate it with the necessary source for the library
    execute_process(
        COMMAND git submodule update --init godot-cpp
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
add_subdirectory(godot-cpp SYSTEM)

# Add godot-cpp's module path and include the exported functions.
# This is made available for documentation generation
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH};${godot-cpp_SOURCE_DIR}/cmake")
include(GodotCPPModule)

# The godot-cpp target has some of useful properties attached that can be retrieved like so.
get_target_property(GODOTCPP_SUFFIX godot::cpp GODOTCPP_SUFFIX)
get_target_property(GODOTCPP_PLATFORM godot::cpp GODOTCPP_PLATFORM)

# Now we can specify our own project which will inherit any global cmake properties or variables that have been defined.
project(ffmpeg_gd
    VERSION 1.0
    DESCRIPTION "FFmpeg video playback GDExtension for Godot."
    LANGUAGES CXX
)

add_library(${LIBNAME} SHARED)

target_sources(${LIBNAME}
    PRIVATE
    src/register_types.cpp
    src/register_types.h
    src/ffmpeg_player_cpu.cpp
    src/ffmpeg_player.cpp
    src/ffmpeg_player_d3d12.cpp
    src/ffmpeg_player.h
    src/ffmpeg_player_platform_stubs.cpp
)

if(UNIX AND NOT APPLE)
    target_sources(${LIBNAME}
        PRIVATE
        src/ffmpeg_player_vulkan.cpp
    )
endif()

if(APPLE)
    enable_language(OBJCXX)
    target_sources(${LIBNAME}
        PRIVATE
        src/ffmpeg_player_videotoolbox.mm
    )
endif()

# Fetch a list of the xml files to use for documentation and add to our target
file(GLOB_RECURSE DOC_XML LIST_DIRECTORIES NO CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/doc_classes/*.xml")

# conditionally add doc data to compile output
if(DOC_XML)
    if(GODOTCPP_TARGET MATCHES "editor|template_debug")
        target_doc_sources(${LIBNAME} ${DOC_XML})
    endif()
endif()

target_link_libraries(${LIBNAME} PRIVATE godot-cpp)

if(FFMPEG_ROOT)
    target_include_directories(${LIBNAME} PRIVATE "${FFMPEG_ROOT}/include")
    target_link_directories(${LIBNAME} PRIVATE "${FFMPEG_ROOT}/lib")
    target_link_libraries(${LIBNAME} PRIVATE avcodec avformat avutil swscale swresample)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)
    target_link_libraries(${LIBNAME} PRIVATE PkgConfig::FFMPEG)
endif()

if(WIN32)
    target_link_libraries(${LIBNAME} PRIVATE d3d12 d3dcompiler vulkan-1)
elseif(APPLE)
    target_link_libraries(${LIBNAME} PRIVATE
        "-framework VideoToolbox"
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework Metal"
        "-framework IOSurface"
        "-framework Foundation"
    )
elseif(UNIX)
    target_link_libraries(${LIBNAME} PRIVATE vulkan)
endif()

# Require at least C++17 for this target
set_property(TARGET ${LIBNAME} PROPERTY CXX_STANDARD 17)

set_target_properties(${LIBNAME}
    PROPERTIES
    # The generator expression here prevents msvc from adding a Debug or Release subdir.
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"

    PREFIX ""
    OUTPUT_NAME "${LIBNAME}${GODOTCPP_SUFFIX}"
)

set(GODOT_PROJECT_BINARY_DIR "${PROJECT_SOURCE_DIR}/${GODOT_PROJECT_DIR}/bin/${GODOTCPP_PLATFORM}")

add_custom_command(TARGET ${LIBNAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${LIBNAME}>" "${GODOT_PROJECT_BINARY_DIR}/$<TARGET_FILE_NAME:${LIBNAME}>"
)

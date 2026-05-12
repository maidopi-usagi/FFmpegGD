#include "ffmpeg_player.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#ifdef FFMPEGGD_HAS_MEDIACODEC
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <jni.h>
#endif

#ifdef FFMPEGGD_HAS_MEDIACODEC
namespace {

JavaVM *ffmpeggd_java_vm = nullptr;

JNIEnv *get_jni_env() {
	if (!ffmpeggd_java_vm) {
		return nullptr;
	}

	JNIEnv *env = nullptr;
	const jint get_env_result = ffmpeggd_java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
	if (get_env_result == JNI_OK) {
		return env;
	}
	if (get_env_result == JNI_EDETACHED) {
		return ffmpeggd_java_vm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
	}
	return nullptr;
}

void clear_exception(JNIEnv *env, const char *context) {
	if (!env || !env->ExceptionCheck()) {
		return;
	}
	env->ExceptionDescribe();
	env->ExceptionClear();
	godot::UtilityFunctions::print("Android JNI exception while ", context, ".");
}

} // namespace

extern "C" jint JNI_OnLoad(JavaVM *vm, void *) {
	ffmpeggd_java_vm = vm;
	const int ffmpeg_jni_result = av_jni_set_java_vm(vm, nullptr);
	if (ffmpeg_jni_result < 0) {
		__android_log_print(ANDROID_LOG_WARN, "FFmpegGD", "Failed to register JavaVM with FFmpeg: %d", ffmpeg_jni_result);
	}
	return JNI_VERSION_1_6;
}
#endif

bool FFmpegPlayer::init_android_mediacodec_context(AVCodecContext *ctx) {
#ifdef FFMPEGGD_HAS_MEDIACODEC
	if (!ctx || texture_external.is_null()) {
		UtilityFunctions::print("Android MediaCodec missing codec context or ExternalTexture.");
		return false;
	}
	JNIEnv *env = get_jni_env();
	if (!env) {
		UtilityFunctions::print("Android MediaCodec will use NDK output buffers because no JNI environment is available.");
		return hw_decoder_init(ctx, AV_HWDEVICE_TYPE_MEDIACODEC) >= 0;
	}

	cleanup_android_resources();

	const int texture_width = ctx->width > 0 ? ctx->width : 256;
	const int texture_height = ctx->height > 0 ? ctx->height : 256;
	texture_external->set_size(Vector2(texture_width, texture_height));
	const uint64_t external_texture_id = texture_external->get_external_texture_id();
	if (external_texture_id == 0) {
		// Godot 4.6 Vulkan ExternalTexture support is still tracked by draft PR #97163,
		// so Vulkan can hard-decode with MediaCodec but cannot use this zero-copy SurfaceTexture path yet.
		UtilityFunctions::print("Android MediaCodec ExternalTexture is unavailable; using NDK output buffers.");
		return hw_decoder_init(ctx, AV_HWDEVICE_TYPE_MEDIACODEC) >= 0;
	}

	jclass surface_texture_class = env->FindClass("android/graphics/SurfaceTexture");
	if (!surface_texture_class) {
		clear_exception(env, "finding SurfaceTexture");
		return false;
	}
	jmethodID surface_texture_ctor = env->GetMethodID(surface_texture_class, "<init>", "(I)V");
	jmethodID set_default_buffer_size = env->GetMethodID(surface_texture_class, "setDefaultBufferSize", "(II)V");
	jmethodID update_tex_image = env->GetMethodID(surface_texture_class, "updateTexImage", "()V");
	if (!surface_texture_ctor || !set_default_buffer_size || !update_tex_image) {
		clear_exception(env, "resolving SurfaceTexture methods");
		env->DeleteLocalRef(surface_texture_class);
		return false;
	}

	jobject surface_texture = env->NewObject(surface_texture_class, surface_texture_ctor, (jint)external_texture_id);
	if (!surface_texture) {
		clear_exception(env, "creating SurfaceTexture");
		env->DeleteLocalRef(surface_texture_class);
		return false;
	}
	env->CallVoidMethod(surface_texture, set_default_buffer_size, (jint)texture_width, (jint)texture_height);
	clear_exception(env, "setting SurfaceTexture buffer size");
	android_surface_texture = env->NewGlobalRef(surface_texture);
	env->DeleteLocalRef(surface_texture);
	env->DeleteLocalRef(surface_texture_class);
	if (!android_surface_texture) {
		UtilityFunctions::print("Failed to keep Android SurfaceTexture reference.");
		return false;
	}

	jclass surface_class = env->FindClass("android/view/Surface");
	if (!surface_class) {
		clear_exception(env, "finding Surface");
		cleanup_android_resources();
		return false;
	}
	jmethodID surface_ctor = env->GetMethodID(surface_class, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
	if (!surface_ctor) {
		clear_exception(env, "resolving Surface constructor");
		env->DeleteLocalRef(surface_class);
		cleanup_android_resources();
		return false;
	}
	jobject surface = env->NewObject(surface_class, surface_ctor, (jobject)android_surface_texture);
	env->DeleteLocalRef(surface_class);
	if (!surface) {
		clear_exception(env, "creating Surface");
		cleanup_android_resources();
		return false;
	}
	android_surface = env->NewGlobalRef(surface);
	env->DeleteLocalRef(surface);
	if (!android_surface) {
		UtilityFunctions::print("Failed to keep Android Surface reference.");
		cleanup_android_resources();
		return false;
	}

	android_mediacodec_context = av_mediacodec_alloc_context();
	if (!android_mediacodec_context) {
		UtilityFunctions::print("Failed to allocate FFmpeg MediaCodec context.");
		cleanup_android_resources();
		return false;
	}
	const int media_codec_init_result = av_mediacodec_default_init(ctx, android_mediacodec_context, android_surface);
	if (media_codec_init_result < 0) {
		char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(media_codec_init_result, error_buffer, sizeof(error_buffer));
		clear_exception(env, "initializing FFmpeg MediaCodec surface context");
		UtilityFunctions::print("Failed to initialize FFmpeg MediaCodec surface context: ", media_codec_init_result, " (", error_buffer, ").");
		cleanup_android_resources();
		return false;
	}

	android_external_texture_ready = false;
	return true;
#else
	return false;
#endif
}

bool FFmpegPlayer::upload_android_mediacodec_frame(AVFrame *src_frame) {
#ifdef FFMPEGGD_HAS_MEDIACODEC
	if (!src_frame || src_frame->format != AV_PIX_FMT_MEDIACODEC || !android_surface_texture || texture_external.is_null()) {
		return false;
	}
	AVMediaCodecBuffer *buffer = reinterpret_cast<AVMediaCodecBuffer *>(src_frame->data[3]);
	if (!buffer) {
		return false;
	}
	if (av_mediacodec_release_buffer(buffer, 1) < 0) {
		UtilityFunctions::print("Failed to render MediaCodec output buffer.");
		return false;
	}

	JNIEnv *env = get_jni_env();
	if (!env) {
		return false;
	}
	jclass surface_texture_class = env->GetObjectClass((jobject)android_surface_texture);
	if (!surface_texture_class) {
		clear_exception(env, "getting SurfaceTexture class");
		return false;
	}
	jmethodID update_tex_image = env->GetMethodID(surface_texture_class, "updateTexImage", "()V");
	if (!update_tex_image) {
		clear_exception(env, "resolving SurfaceTexture.updateTexImage");
		env->DeleteLocalRef(surface_texture_class);
		return false;
	}
	env->CallVoidMethod((jobject)android_surface_texture, update_tex_image);
	if (env->ExceptionCheck()) {
		clear_exception(env, "updating SurfaceTexture image");
		env->DeleteLocalRef(surface_texture_class);
		return false;
	}
	env->DeleteLocalRef(surface_texture_class);
	android_external_texture_ready = true;
	return true;
#else
	return false;
#endif
}

void FFmpegPlayer::cleanup_android_resources() {
#ifdef FFMPEGGD_HAS_MEDIACODEC
	if (android_mediacodec_context && decoder_ctx) {
		av_mediacodec_default_free(decoder_ctx);
	}
	android_mediacodec_context = nullptr;

	JNIEnv *env = get_jni_env();
	if (env) {
		if (android_surface) {
			jclass surface_class = env->GetObjectClass((jobject)android_surface);
			jmethodID release = surface_class ? env->GetMethodID(surface_class, "release", "()V") : nullptr;
			if (release) {
				env->CallVoidMethod((jobject)android_surface, release);
				clear_exception(env, "releasing Surface");
			}
			if (surface_class) env->DeleteLocalRef(surface_class);
			env->DeleteGlobalRef((jobject)android_surface);
		}
		if (android_surface_texture) {
			jclass surface_texture_class = env->GetObjectClass((jobject)android_surface_texture);
			jmethodID release = surface_texture_class ? env->GetMethodID(surface_texture_class, "release", "()V") : nullptr;
			if (release) {
				env->CallVoidMethod((jobject)android_surface_texture, release);
				clear_exception(env, "releasing SurfaceTexture");
			}
			if (surface_texture_class) env->DeleteLocalRef(surface_texture_class);
			env->DeleteGlobalRef((jobject)android_surface_texture);
		}
	}
	android_surface = nullptr;
	android_surface_texture = nullptr;
	android_native_surface_texture = nullptr;
	android_external_texture_ready = false;
	if (android_hardware_buffer) {
		AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer *>(android_hardware_buffer));
		android_hardware_buffer = nullptr;
	}
#else
	android_surface_texture = nullptr;
	android_surface = nullptr;
	android_native_surface_texture = nullptr;
	android_hardware_buffer = nullptr;
	android_mediacodec_context = nullptr;
	android_external_texture_ready = false;
#endif
}

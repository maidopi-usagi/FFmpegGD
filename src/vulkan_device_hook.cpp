#include "vulkan_device_hook.h"

#if defined(_WIN32) && __has_include(<vulkan/vulkan.h>)

#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

using VkCreateDeviceFn = VkResult(VKAPI_PTR *)(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
using VkGetInstanceProcAddrFn = PFN_vkVoidFunction(VKAPI_PTR *)(VkInstance, const char *);

VkCreateDeviceFn original_vk_create_device = nullptr;
VkGetInstanceProcAddrFn original_vk_get_instance_proc_addr = nullptr;
std::once_flag install_once;
std::mutex detour_mutex;
uint32_t injected_video_queue_family = UINT32_MAX;
constexpr size_t VK_CREATE_DEVICE_PATCH_SIZE = 12;
constexpr VkQueueFlags VK_VIDEO_DECODE_QUEUE_FLAG_FALLBACK = 0x00000020;
uint8_t original_vk_create_device_bytes[VK_CREATE_DEVICE_PATCH_SIZE] = {};
bool inline_hook_installed = false;

VkResult VKAPI_PTR hooked_vk_create_device(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkDevice *device);

void hook_log(const char *message) {
	std::fprintf(stderr, "%s\n", message);
	std::fflush(stderr);
	OutputDebugStringA(message);
	OutputDebugStringA("\n");
}

bool has_extension(const VkDeviceCreateInfo *info, const char *name) {
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (info->ppEnabledExtensionNames[i] && std::strcmp(info->ppEnabledExtensionNames[i], name) == 0) return true;
	}
	return false;
}

bool write_absolute_jump(void *target, void *replacement) {
	uint8_t patch[VK_CREATE_DEVICE_PATCH_SIZE] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
	std::memcpy(patch + 2, &replacement, sizeof(replacement));

	DWORD old_protect = 0;
	if (!VirtualProtect(target, VK_CREATE_DEVICE_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
	std::memcpy(target, patch, sizeof(patch));
	FlushInstructionCache(GetCurrentProcess(), target, VK_CREATE_DEVICE_PATCH_SIZE);
	VirtualProtect(target, VK_CREATE_DEVICE_PATCH_SIZE, old_protect, &old_protect);
	return true;
}

bool restore_original_vk_create_device() {
	if (!original_vk_create_device) return false;
	DWORD old_protect = 0;
	void *target = reinterpret_cast<void *>(original_vk_create_device);
	if (!VirtualProtect(target, VK_CREATE_DEVICE_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
	std::memcpy(target, original_vk_create_device_bytes, VK_CREATE_DEVICE_PATCH_SIZE);
	FlushInstructionCache(GetCurrentProcess(), target, VK_CREATE_DEVICE_PATCH_SIZE);
	VirtualProtect(target, VK_CREATE_DEVICE_PATCH_SIZE, old_protect, &old_protect);
	return true;
}

VkResult call_original_vk_create_device(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkDevice *device) {
	std::lock_guard<std::mutex> lock(detour_mutex);
	if (!inline_hook_installed) {
		return original_vk_create_device(physical_device, create_info, allocator, device);
	}
	restore_original_vk_create_device();
	VkResult result = original_vk_create_device(physical_device, create_info, allocator, device);
	write_absolute_jump(reinterpret_cast<void *>(original_vk_create_device), reinterpret_cast<void *>(hooked_vk_create_device));
	return result;
}

bool supports_extension(VkPhysicalDevice physical_device, const char *name) {
	auto enumerate_extensions = (PFN_vkEnumerateDeviceExtensionProperties)original_vk_get_instance_proc_addr(nullptr, "vkEnumerateDeviceExtensionProperties");
	if (!enumerate_extensions) {
		HMODULE vulkan_loader = GetModuleHandleA("vulkan-1.dll");
		enumerate_extensions = vulkan_loader ? (PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(vulkan_loader, "vkEnumerateDeviceExtensionProperties") : nullptr;
	}
	if (!enumerate_extensions) return false;

	uint32_t count = 0;
	if (enumerate_extensions(physical_device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) return false;
	std::vector<VkExtensionProperties> properties(count);
	if (enumerate_extensions(physical_device, nullptr, &count, properties.data()) != VK_SUCCESS) return false;
	for (const VkExtensionProperties &property : properties) {
		if (std::strcmp(property.extensionName, name) == 0) return true;
	}
	return false;
}

uint32_t find_video_decode_queue_family(VkPhysicalDevice physical_device) {
	auto get_queue_family_properties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)original_vk_get_instance_proc_addr(nullptr, "vkGetPhysicalDeviceQueueFamilyProperties2");
	if (!get_queue_family_properties2) {
		HMODULE vulkan_loader = GetModuleHandleA("vulkan-1.dll");
		get_queue_family_properties2 = vulkan_loader ? (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)GetProcAddress(vulkan_loader, "vkGetPhysicalDeviceQueueFamilyProperties2") : nullptr;
	}
	if (get_queue_family_properties2) {
		uint32_t count = 0;
		get_queue_family_properties2(physical_device, &count, nullptr);
		if (count > 0) {
			std::vector<VkQueueFamilyProperties2> properties(count);
#ifdef VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR
			std::vector<VkQueueFamilyVideoPropertiesKHR> video_properties(count);
#endif
			for (uint32_t i = 0; i < count; i++) {
				properties[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
#ifdef VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR
				video_properties[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
				properties[i].pNext = &video_properties[i];
#endif
			}
			get_queue_family_properties2(physical_device, &count, properties.data());
			for (uint32_t i = 0; i < count; i++) {
				char message[256] = {};
#ifdef VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR
				std::snprintf(message, sizeof(message), "[FFmpegGD/VulkanHook] Queue family %u flags=0x%llx video_ops=0x%llx", i, (unsigned long long)properties[i].queueFamilyProperties.queueFlags, (unsigned long long)video_properties[i].videoCodecOperations);
#else
				std::snprintf(message, sizeof(message), "[FFmpegGD/VulkanHook] Queue family %u flags=0x%llx", i, (unsigned long long)properties[i].queueFamilyProperties.queueFlags);
#endif
				hook_log(message);
				if (properties[i].queueFamilyProperties.queueFlags & VK_VIDEO_DECODE_QUEUE_FLAG_FALLBACK) return i;
#ifdef VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR
				const VkVideoCodecOperationFlagsKHR ops = video_properties[i].videoCodecOperations;
#ifdef VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR
				if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) return i;
#endif
#ifdef VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR
				if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) return i;
#endif
#ifdef VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR
				if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) return i;
#endif
#endif
			}
		}
	}

	auto get_queue_family_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)original_vk_get_instance_proc_addr(nullptr, "vkGetPhysicalDeviceQueueFamilyProperties");
	if (!get_queue_family_properties) {
		HMODULE vulkan_loader = GetModuleHandleA("vulkan-1.dll");
		get_queue_family_properties = vulkan_loader ? (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(vulkan_loader, "vkGetPhysicalDeviceQueueFamilyProperties") : nullptr;
	}
	if (!get_queue_family_properties) return UINT32_MAX;

	uint32_t count = 0;
	get_queue_family_properties(physical_device, &count, nullptr);
	if (count == 0) return UINT32_MAX;
	std::vector<VkQueueFamilyProperties> properties(count);
	get_queue_family_properties(physical_device, &count, properties.data());
	for (uint32_t i = 0; i < count; i++) {
		if (properties[i].queueFlags & VK_VIDEO_DECODE_QUEUE_FLAG_FALLBACK) return i;
	}
	return UINT32_MAX;
}

bool queue_family_already_requested(const VkDeviceCreateInfo *info, uint32_t family) {
	for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
		if (info->pQueueCreateInfos[i].queueFamilyIndex == family) return true;
	}
	return false;
}

VkResult VKAPI_PTR hooked_vk_create_device(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkDevice *device) {
	hook_log("[FFmpegGD/VulkanHook] Intercepted vkCreateDevice.");
	if (!original_vk_create_device || !create_info) {
		return original_vk_create_device ? call_original_vk_create_device(physical_device, create_info, allocator, device) : VK_ERROR_INITIALIZATION_FAILED;
	}

	std::vector<const char *> extensions;
	extensions.reserve(create_info->enabledExtensionCount + 8);
	for (uint32_t i = 0; i < create_info->enabledExtensionCount; i++) {
		extensions.push_back(create_info->ppEnabledExtensionNames[i]);
	}

	auto add_extension = [&](const char *name) {
		if (!has_extension(create_info, name) && supports_extension(physical_device, name)) {
			extensions.push_back(name);
			char message[256] = {};
			std::snprintf(message, sizeof(message), "[FFmpegGD/VulkanHook] Adding device extension: %s", name);
			hook_log(message);
			return true;
		}
		return false;
	};

	bool injected_any = false;
	injected_any |= add_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#ifdef VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
#endif
#ifdef VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
#endif
#ifdef VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#endif
#ifdef VK_KHR_VIDEO_QUEUE_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
#endif
#ifdef VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
#endif
#ifdef VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
#endif
#ifdef VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
#endif
#ifdef VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME
	injected_any |= add_extension(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
#endif

	std::vector<VkDeviceQueueCreateInfo> queues;
	queues.assign(create_info->pQueueCreateInfos, create_info->pQueueCreateInfos + create_info->queueCreateInfoCount);
	static const float video_queue_priority = 1.0f;
	const uint32_t video_family = find_video_decode_queue_family(physical_device);
	if (video_family != UINT32_MAX && !queue_family_already_requested(create_info, video_family)) {
		VkDeviceQueueCreateInfo queue_info = {};
		queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_info.queueFamilyIndex = video_family;
		queue_info.queueCount = 1;
		queue_info.pQueuePriorities = &video_queue_priority;
		queues.push_back(queue_info);
		injected_video_queue_family = video_family;
		injected_any = true;
		char message[256] = {};
		std::snprintf(message, sizeof(message), "[FFmpegGD/VulkanHook] Adding video decode queue family: %u", video_family);
		hook_log(message);
	} else if (video_family == UINT32_MAX) {
		hook_log("[FFmpegGD/VulkanHook] No VK_QUEUE_VIDEO_DECODE_BIT_KHR queue family reported.");
	} else {
		injected_video_queue_family = video_family;
		char message[256] = {};
		std::snprintf(message, sizeof(message), "[FFmpegGD/VulkanHook] Video decode queue family already requested: %u", video_family);
		hook_log(message);
	}

	VkDeviceCreateInfo modified = *create_info;
	modified.enabledExtensionCount = (uint32_t)extensions.size();
	modified.ppEnabledExtensionNames = extensions.data();
	modified.queueCreateInfoCount = (uint32_t)queues.size();
	modified.pQueueCreateInfos = queues.data();

	if (injected_any) {
		hook_log("[FFmpegGD/VulkanHook] Injecting Vulkan external/video decode device requirements.");
	}

	VkResult result = call_original_vk_create_device(physical_device, &modified, allocator, device);
	if (result == VK_SUCCESS && injected_any) {
		hook_log("[FFmpegGD/VulkanHook] vkCreateDevice succeeded with injected requirements.");
	} else if (injected_any) {
		hook_log("[FFmpegGD/VulkanHook] vkCreateDevice failed after injection.");
	}
	return result;
}

PFN_vkVoidFunction VKAPI_PTR hooked_vk_get_instance_proc_addr(VkInstance instance, const char *name) {
	if (name && std::strcmp(name, "vkCreateDevice") == 0) {
		if (!original_vk_create_device && original_vk_get_instance_proc_addr) {
			original_vk_create_device = (VkCreateDeviceFn)original_vk_get_instance_proc_addr(instance, name);
		}
		return (PFN_vkVoidFunction)hooked_vk_create_device;
	}
	return original_vk_get_instance_proc_addr ? original_vk_get_instance_proc_addr(instance, name) : nullptr;
}

bool install_inline_vk_create_device_hook(HMODULE vulkan_loader) {
	original_vk_get_instance_proc_addr = (VkGetInstanceProcAddrFn)GetProcAddress(vulkan_loader, "vkGetInstanceProcAddr");
	original_vk_create_device = (VkCreateDeviceFn)GetProcAddress(vulkan_loader, "vkCreateDevice");
	if (!original_vk_get_instance_proc_addr || !original_vk_create_device) return false;
	std::memcpy(original_vk_create_device_bytes, reinterpret_cast<void *>(original_vk_create_device), VK_CREATE_DEVICE_PATCH_SIZE);
	if (!write_absolute_jump(reinterpret_cast<void *>(original_vk_create_device), reinterpret_cast<void *>(hooked_vk_create_device))) return false;
	inline_hook_installed = true;
	hook_log("[FFmpegGD/VulkanHook] Installed inline vkCreateDevice hook.");
	return true;
}

} // namespace

void ffmpeggd_install_vulkan_device_hook() {
	std::call_once(install_once, []() {
		char enabled[8] = {};
		if (GetEnvironmentVariableA("FFMPEGGD_EXPERIMENTAL_VULKAN_INLINE_HOOK", enabled, sizeof(enabled)) == 0 || std::strcmp(enabled, "1") != 0) {
			return;
		}
		HMODULE vulkan_loader = LoadLibraryA("vulkan-1.dll");
		if (!vulkan_loader || !install_inline_vk_create_device_hook(vulkan_loader)) {
			hook_log("[FFmpegGD/VulkanHook] Failed to install inline vkCreateDevice hook.");
		}
	});
}

uint32_t ffmpeggd_get_vulkan_video_queue_family() {
	return injected_video_queue_family;
}

#else

void ffmpeggd_install_vulkan_device_hook() {
}

uint32_t ffmpeggd_get_vulkan_video_queue_family() {
	return UINT32_MAX;
}

#endif

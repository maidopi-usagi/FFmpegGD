#include "ffmpeg_player.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

void FFmpegPlayer::cleanup_vulkan_resources() {
	if (vk_device && vk_command_pool) {
		vkDestroyCommandPool(vk_device, vk_command_pool, nullptr);
	}
	vk_command_pool = VK_NULL_HANDLE;
	vk_command_buffer = VK_NULL_HANDLE;
	vk_queue = VK_NULL_HANDLE;
	vk_device = VK_NULL_HANDLE;
	vk_phys_device = VK_NULL_HANDLE;
	vk_instance = VK_NULL_HANDLE;
	vk_queue_family_index = 0;
	using_godot_vulkan_device = false;
	using_godot_d3d12_device = false;
}

bool FFmpegPlayer::init_vulkan_context() {
	cleanup_vulkan_resources();
	if (hw_device_ctx) {
		av_buffer_unref(&hw_device_ctx);
	}
	if (!rd) return false;

	// Get Vulkan handles from Godot
	vk_device = (VkDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_DEVICE, RID(), 0);
	vk_phys_device = (VkPhysicalDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_PHYSICAL_DEVICE, RID(), 0);
	vk_instance = (VkInstance)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_INSTANCE, RID(), 0);
	vk_queue = (VkQueue)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_QUEUE, RID(), 0);
	vk_queue_family_index = (uint32_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_QUEUE_FAMILY_INDEX, RID(), 0);

	if (!vk_device || !vk_phys_device || !vk_instance) {
		UtilityFunctions::print("Failed to get Vulkan handles from Godot. Is the renderer Vulkan?");
		return false;
	}

	// Create AVHWDeviceContext for Vulkan
	hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
	if (!hw_device_ctx) return false;

	AVHWDeviceContext *device_ctx = (AVHWDeviceContext *)hw_device_ctx->data;
	AVVulkanDeviceContext *vk_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;

	vk_ctx->inst = vk_instance;
	vk_ctx->phys_dev = vk_phys_device;
	vk_ctx->act_dev = vk_device;
	vk_ctx->get_proc_addr = vkGetInstanceProcAddr;
	
	vk_ctx->qf[0].idx = vk_queue_family_index;
	vk_ctx->qf[0].num = 1;
	vk_ctx->qf[0].flags = (VkQueueFlagBits)(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
	vk_ctx->qf[0].video_caps = (VkVideoCodecOperationFlagBitsKHR)0;
	vk_ctx->nb_qf = 1;
	
	int ret = av_hwdevice_ctx_init(hw_device_ctx);
	if (ret < 0) {
		UtilityFunctions::print("Failed to initialize FFmpeg Vulkan context: ", ret);
		av_buffer_unref(&hw_device_ctx);
		return false;
	}

	// Create command pool for our copy operations
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = vk_queue_family_index;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	if (vkCreateCommandPool(vk_device, &poolInfo, nullptr, &vk_command_pool) != VK_SUCCESS) {
		UtilityFunctions::print("Failed to create command pool");
		av_buffer_unref(&hw_device_ctx);
		cleanup_vulkan_resources();
		return false;
	}

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = vk_command_pool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	if (vkAllocateCommandBuffers(vk_device, &allocInfo, &vk_command_buffer) != VK_SUCCESS) {
		UtilityFunctions::print("Failed to allocate command buffer");
		av_buffer_unref(&hw_device_ctx);
		cleanup_vulkan_resources();
		return false;
	}

	using_godot_vulkan_device = true;
	return true;
}

void FFmpegPlayer::copy_vulkan_image(AVFrame *src_frame) {
	if (!rd || !vk_command_buffer || !using_godot_vulkan_device) return;

	AVVkFrame *vk_frame = (AVVkFrame *)src_frame->data[0];
	if (!vk_frame || !vk_frame->img[0]) return;
	VkImage src_image = vk_frame->img[0];
	
	// Ensure destination textures exist
	int width = src_frame->width;
	int height = src_frame->height;
	bool is_planar = (src_frame->format == AV_PIX_FMT_VULKAN && (decoder_ctx->sw_pix_fmt == AV_PIX_FMT_YUV420P || decoder_ctx->sw_pix_fmt == AV_PIX_FMT_YUVJ420P));
	is_planar_texture = is_planar;

	if (!texture_y_rid.is_valid() || texture_y->get_width() != width || texture_y->get_height() != height) {
		if (texture_y_rid.is_valid()) rd->free_rid(texture_y_rid);
		if (texture_uv_rid.is_valid()) rd->free_rid(texture_uv_rid);
		if (texture_v_rid.is_valid()) rd->free_rid(texture_v_rid);

		// Create Y texture (R8)
		Ref<RDTextureFormat> fmt_y;
		fmt_y.instantiate();
		fmt_y->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		fmt_y->set_width(width);
		fmt_y->set_height(height);
		fmt_y->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		fmt_y->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

		Ref<RDTextureView> view;
		view.instantiate();
		texture_y_rid = rd->texture_create(fmt_y, view);
		texture_y->set_texture_rd_rid(texture_y_rid);

		if (is_planar) {
			// YUV420P: U and V are separate planes, R8 each
			Ref<RDTextureFormat> fmt_u;
			fmt_u.instantiate();
			fmt_u->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
			fmt_u->set_width(width / 2);
			fmt_u->set_height(height / 2);
			fmt_u->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
			fmt_u->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

			Ref<RDTextureFormat> fmt_v;
			fmt_v.instantiate();
			fmt_v->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
			fmt_v->set_width(width / 2);
			fmt_v->set_height(height / 2);
			fmt_v->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
			fmt_v->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

			texture_uv_rid = rd->texture_create(fmt_u, view); // Reuse texture_uv for U
			texture_v_rid = rd->texture_create(fmt_v, view);

			texture_uv->set_texture_rd_rid(texture_uv_rid);
			texture_v->set_texture_rd_rid(texture_v_rid);
		} else {
			// NV12: UV is one plane, R8G8
			Ref<RDTextureFormat> fmt_uv;
			fmt_uv.instantiate();
			fmt_uv->set_format(RenderingDevice::DATA_FORMAT_R8G8_UNORM);
			fmt_uv->set_width(width / 2);
			fmt_uv->set_height(height / 2);
			fmt_uv->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
			fmt_uv->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

			texture_uv_rid = rd->texture_create(fmt_uv, view);
			texture_uv->set_texture_rd_rid(texture_uv_rid);
		}
	}

	// Get destination VkImages
	VkImage dst_image_y = (VkImage)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_IMAGE, texture_y_rid, 0);
	VkImage dst_image_uv = (VkImage)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_IMAGE, texture_uv_rid, 0);
	VkImage dst_image_v = VK_NULL_HANDLE;
	if (is_planar) {
		dst_image_v = (VkImage)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_VULKAN_IMAGE, texture_v_rid, 0);
	}
	if (!dst_image_y || !dst_image_uv || (is_planar && !dst_image_v)) return;

	vkResetCommandBuffer(vk_command_buffer, 0);

	// Record command buffer
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	if (vkBeginCommandBuffer(vk_command_buffer, &beginInfo) != VK_SUCCESS) return;

	const VkImageLayout old_src_layout = vk_frame->layout[0];
	const VkAccessFlags old_src_access = vk_frame->access[0];
	const VkImageAspectFlags src_aspects = is_planar ?
		(VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT) :
		(VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);

	// Barriers to transition layouts
	int barrier_count = is_planar ? 4 : 3;
	VkImageMemoryBarrier barriers[4] = {};
	
	// Src Barrier
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = old_src_layout;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = src_image;
	barriers[0].subresourceRange.aspectMask = src_aspects;
	barriers[0].subresourceRange.baseMipLevel = 0;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.baseArrayLayer = 0;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = old_src_access;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	// Dst Y Barrier
	barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].image = dst_image_y;
	barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[1].subresourceRange.baseMipLevel = 0;
	barriers[1].subresourceRange.levelCount = 1;
	barriers[1].subresourceRange.baseArrayLayer = 0;
	barriers[1].subresourceRange.layerCount = 1;
	barriers[1].srcAccessMask = 0;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	// Dst UV/U Barrier
	barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[2].image = dst_image_uv;
	barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[2].subresourceRange.baseMipLevel = 0;
	barriers[2].subresourceRange.levelCount = 1;
	barriers[2].subresourceRange.baseArrayLayer = 0;
	barriers[2].subresourceRange.layerCount = 1;
	barriers[2].srcAccessMask = 0;
	barriers[2].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	if (is_planar) {
		// Dst V Barrier
		barriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[3].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[3].image = dst_image_v;
		barriers[3].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[3].subresourceRange.baseMipLevel = 0;
		barriers[3].subresourceRange.levelCount = 1;
		barriers[3].subresourceRange.baseArrayLayer = 0;
		barriers[3].subresourceRange.layerCount = 1;
		barriers[3].srcAccessMask = 0;
		barriers[3].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	}

	vkCmdPipelineBarrier(vk_command_buffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, // Dest stages
		0,
		0, nullptr,
		0, nullptr,
		barrier_count, barriers
	);

	// Copy Y Plane
	VkImageCopy copyRegionY{};
	copyRegionY.srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	copyRegionY.srcSubresource.layerCount = 1;
	copyRegionY.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegionY.dstSubresource.layerCount = 1;
	copyRegionY.extent.width = width;
	copyRegionY.extent.height = height;
	copyRegionY.extent.depth = 1;

	vkCmdCopyImage(vk_command_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image_y, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegionY);

	if (is_planar) {
		// Copy U Plane
		VkImageCopy copyRegionU{};
		copyRegionU.srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		copyRegionU.srcSubresource.layerCount = 1;
		copyRegionU.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegionU.dstSubresource.layerCount = 1;
		copyRegionU.extent.width = width / 2;
		copyRegionU.extent.height = height / 2;
		copyRegionU.extent.depth = 1;

		vkCmdCopyImage(vk_command_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image_uv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegionU);

		// Copy V Plane
		VkImageCopy copyRegionV{};
		copyRegionV.srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
		copyRegionV.srcSubresource.layerCount = 1;
		copyRegionV.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegionV.dstSubresource.layerCount = 1;
		copyRegionV.extent.width = width / 2;
		copyRegionV.extent.height = height / 2;
		copyRegionV.extent.depth = 1;

		vkCmdCopyImage(vk_command_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image_v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegionV);
	} else {
		// Copy UV Plane
		VkImageCopy copyRegionUV{};
		copyRegionUV.srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		copyRegionUV.srcSubresource.layerCount = 1;
		copyRegionUV.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegionUV.dstSubresource.layerCount = 1;
		copyRegionUV.extent.width = width / 2;
		copyRegionUV.extent.height = height / 2;
		copyRegionUV.extent.depth = 1;

		vkCmdCopyImage(vk_command_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image_uv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegionUV);
	}

	// Restore FFmpeg's source image state and make destination images shader-readable.
	
	int barrier_end_count = is_planar ? 4 : 3;
	VkImageMemoryBarrier barriers_end[4] = {};

	barriers_end[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers_end[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers_end[0].newLayout = old_src_layout;
	barriers_end[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[0].image = src_image;
	barriers_end[0].subresourceRange.aspectMask = src_aspects;
	barriers_end[0].subresourceRange.baseMipLevel = 0;
	barriers_end[0].subresourceRange.levelCount = 1;
	barriers_end[0].subresourceRange.baseArrayLayer = 0;
	barriers_end[0].subresourceRange.layerCount = 1;
	barriers_end[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers_end[0].dstAccessMask = old_src_access;
	
	barriers_end[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers_end[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers_end[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers_end[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[1].image = dst_image_y;
	barriers_end[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers_end[1].subresourceRange.baseMipLevel = 0;
	barriers_end[1].subresourceRange.levelCount = 1;
	barriers_end[1].subresourceRange.baseArrayLayer = 0;
	barriers_end[1].subresourceRange.layerCount = 1;
	barriers_end[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers_end[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	barriers_end[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers_end[2].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers_end[2].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers_end[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers_end[2].image = dst_image_uv;
	barriers_end[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers_end[2].subresourceRange.baseMipLevel = 0;
	barriers_end[2].subresourceRange.levelCount = 1;
	barriers_end[2].subresourceRange.baseArrayLayer = 0;
	barriers_end[2].subresourceRange.layerCount = 1;
	barriers_end[2].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers_end[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	if (is_planar) {
		barriers_end[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers_end[3].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers_end[3].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barriers_end[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers_end[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers_end[3].image = dst_image_v;
		barriers_end[3].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers_end[3].subresourceRange.baseMipLevel = 0;
		barriers_end[3].subresourceRange.levelCount = 1;
		barriers_end[3].subresourceRange.baseArrayLayer = 0;
		barriers_end[3].subresourceRange.layerCount = 1;
		barriers_end[3].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers_end[3].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}

	vkCmdPipelineBarrier(vk_command_buffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		barrier_end_count, barriers_end
	);

	if (vkEndCommandBuffer(vk_command_buffer) != VK_SUCCESS) return;

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &vk_command_buffer;

	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(vk_device, &fence_info, nullptr, &fence) != VK_SUCCESS) return;
	VkResult submit_result = vkQueueSubmit(vk_queue, 1, &submitInfo, fence);
	if (submit_result == VK_SUCCESS) {
		// TODO: Replace this blocking wait with fence-tracked ring textures once the Vulkan path can be verified.
		vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
	}
	vkDestroyFence(vk_device, fence, nullptr);
}

/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "drv_priv.h"
#include "gbm.h"
#include "helpers.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <magma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_VERBOSE(msg, ...)                                                                      \
	if (false)                                                                                 \
	drv_log(msg, ##__VA_ARGS__)

static magma_connection_t get_connection(struct driver *drv)
{
	return drv->priv;
}

static int magma_init(struct driver *drv)
{
	magma_device_t device;
	magma_connection_t connection;

	magma_status_t status = magma_device_import(drv->fd, &device);
	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_device_import failed: %d", status);
		return NULL;
	}

	status = magma_create_connection2(device, &connection);
	magma_device_release(device);

	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_create_connection2 failed: %d", status);
		return NULL;
	}

	drv->priv = connection;

	const uint32_t formats[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888,
				     DRM_FORMAT_XRGB8888 };

	struct format_metadata metadata;
	metadata.tiling = 0;
	metadata.priority = 0;
	metadata.modifier = 0;

	drv_add_combinations(drv, formats, ARRAY_SIZE(formats), &metadata,
			     BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	return 0;
}

static void magma_close(struct driver *drv)
{
	magma_release_connection(get_connection(drv));
	drv->priv = NULL;
}

static int bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
		     uint64_t use_flags, const uint64_t *modifiers, uint32_t count)
{
	if (count >= MAGMA_MAX_DRM_FORMAT_MODIFIERS) {
		return -EINVAL;
	}

	magma_image_create_info_t create_info = {
		.width = width,
		.height = height,
		.drm_format = format,
		.flags =
		    (use_flags == GBM_BO_USE_SCANOUT) ? MAGMA_IMAGE_CREATE_FLAGS_PRESENTABLE : 0,
	};

	if (use_flags & GBM_BO_USE_LINEAR) {
		create_info.drm_format_modifiers[0] = DRM_FORMAT_MOD_LINEAR;
		create_info.drm_format_modifiers[1] = DRM_FORMAT_MOD_INVALID;
	} else {
		memcpy(create_info.drm_format_modifiers, modifiers, count * sizeof(uint64_t));
		create_info.drm_format_modifiers[count] = DRM_FORMAT_MOD_INVALID;
	}

	magma_buffer_t image;
	magma_status_t status =
	    magma_virt_create_image(get_connection(bo->drv), &create_info, &image);
	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_virt_create_image failed: %d", status);
		return -EINVAL;
	}

	magma_image_info_t info;
	status = magma_virt_get_image_info(get_connection(bo->drv), image, &info);
	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_virt_get_image_info failed: %d", status);
		magma_release_buffer(get_connection(bo->drv), image);
		return -EINVAL;
	}

	bo->meta.total_size = magma_get_buffer_size(image);
	// Only one plane supported.
	bo->meta.sizes[0] = bo->meta.total_size;
	bo->meta.format_modifier = info.drm_format_modifier;
	bo->handles[0].u64 = image;

	for (uint32_t plane = 0; plane < DRV_MAX_PLANES; plane++) {
		bo->meta.offsets[plane] = info.plane_offsets[plane];
		bo->meta.strides[plane] = info.plane_strides[plane];
	}

	return 0;
}

static int magma_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			   uint64_t use_flags)
{
	return bo_create(bo, width, height, format, use_flags, NULL, 0);
}

static int magma_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					  uint32_t format, const uint64_t *modifiers,
					  uint32_t count)
{
	return bo_create(bo, width, height, format, 0 /*use_flags*/, modifiers, count);
}

static int magma_bo_destroy(struct bo *bo)
{
	magma_buffer_t image = bo->handles[0].u64;
	magma_release_buffer(get_connection(bo->drv), image);
	return 0;
}

int magma_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	magma_handle_t handle = data->fds[0];

	magma_buffer_t image;
	magma_status_t status = magma_import(get_connection(bo->drv), handle, &image);
	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_import failed: %d", status);
		return -EINVAL;
	}

	bo->handles[0].u64 = image;
	bo->meta.total_size = magma_get_buffer_size(image);

	return 0;
}

void *magma_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	magma_buffer_t image = bo->handles[0].u64;

	magma_handle_t handle;
	magma_status_t status = magma_get_buffer_handle(get_connection(bo->drv), image, &handle);
	if (status != MAGMA_STATUS_OK) {
		LOG_VERBOSE("magma_get_buffer_handle failed: %d", status);
		return MAP_FAILED;
	}

	int fd = handle;
	size_t length = bo->meta.total_size;

	uint32_t flags = 0;
	if (map_flags & GBM_BO_TRANSFER_READ)
		flags |= PROT_READ;
	if (map_flags & GBM_BO_TRANSFER_WRITE)
		flags |= PROT_WRITE;

	void *addr = mmap(NULL, length, flags, MAP_SHARED, fd, 0 /*offset*/);

	close(fd);

	if (addr == MAP_FAILED) {
		LOG_VERBOSE("mmap failed");
		return MAP_FAILED;
	}

	vma->addr = addr;
	vma->length = length;
	vma->map_flags = map_flags;

	return addr;
};

int magma_bo_unmap(struct bo *bo, struct vma *vma)
{
	return munmap(vma->addr, vma->length);
}

int magma_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	// No cache operation needed for Intel.
	return 0;
}

int magma_bo_flush(struct bo *bo, struct mapping *mapping)
{
	// No cache operation needed for Intel.
	return 0;
}

const struct backend backend_magma = {
	.name = "magma",
	.init = magma_init,
	.close = magma_close,
	.bo_create = magma_bo_create,
	.bo_create_with_modifiers = magma_bo_create_with_modifiers,
	.bo_destroy = magma_bo_destroy,
	.bo_import = magma_bo_import,
	.bo_map = magma_bo_map,
	.bo_unmap = magma_bo_unmap,
	.bo_invalidate = magma_bo_invalidate,
	.bo_flush = magma_bo_flush,
};

// Should this be made a backend function?
int drv_bo_get_plane_fd(struct bo *bo, size_t plane)
{
	if (plane != 0)
		return -1;

	magma_buffer_t image = bo->handles[0].u64;

	magma_handle_t handle;
	magma_status_t status = magma_export(get_connection(bo->drv), image, &handle);
	if (status != MAGMA_STATUS_OK)
		return -1;

	int fd = handle;
	return fd;
}

// Reference counting not needed; each magma import generates a unique magma buffer.
uintptr_t drv_get_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
	// Stub: this is only called after decrementing: if zero then the bo is destroyed.
	return 0;
}

void drv_increment_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
}

void drv_decrement_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
}

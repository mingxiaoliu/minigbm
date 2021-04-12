/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"

struct planar_layout {
	size_t num_planes;
	int horizontal_subsampling[DRV_MAX_PLANES];
	int vertical_subsampling[DRV_MAX_PLANES];
	int bytes_per_pixel[DRV_MAX_PLANES];
};

// clang-format off

static const struct planar_layout packed_1bpp_layout = {
	.num_planes = 1,
	.horizontal_subsampling = { 1 },
	.vertical_subsampling = { 1 },
	.bytes_per_pixel = { 1 }
};

static const struct planar_layout packed_2bpp_layout = {
	.num_planes = 1,
	.horizontal_subsampling = { 1 },
	.vertical_subsampling = { 1 },
	.bytes_per_pixel = { 2 }
};

static const struct planar_layout packed_3bpp_layout = {
	.num_planes = 1,
	.horizontal_subsampling = { 1 },
	.vertical_subsampling = { 1 },
	.bytes_per_pixel = { 3 }
};

static const struct planar_layout packed_4bpp_layout = {
	.num_planes = 1,
	.horizontal_subsampling = { 1 },
	.vertical_subsampling = { 1 },
	.bytes_per_pixel = { 4 }
};

static const struct planar_layout packed_8bpp_layout = {
	.num_planes = 1,
	.horizontal_subsampling = { 1 },
	.vertical_subsampling = { 1 },
	.bytes_per_pixel = { 8 }
};

static const struct planar_layout biplanar_yuv_420_layout = {
	.num_planes = 2,
	.horizontal_subsampling = { 1, 2 },
	.vertical_subsampling = { 1, 2 },
	.bytes_per_pixel = { 1, 2 }
};

static const struct planar_layout triplanar_yuv_420_layout = {
	.num_planes = 3,
	.horizontal_subsampling = { 1, 2, 2 },
	.vertical_subsampling = { 1, 2, 2 },
	.bytes_per_pixel = { 1, 1, 1 }
};

static const struct planar_layout biplanar_yuv_p010_layout = {
	.num_planes = 2,
	.horizontal_subsampling = { 1, 2 },
	.vertical_subsampling = { 1, 2 },
	.bytes_per_pixel = { 2, 4 }
};

// clang-format on

static const struct planar_layout *layout_from_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_BGR233:
	case DRM_FORMAT_C8:
	case DRM_FORMAT_R8:
	case DRM_FORMAT_RGB332:
		return &packed_1bpp_layout;

	case DRM_FORMAT_R16:
		return &packed_2bpp_layout;

	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		return &triplanar_yuv_420_layout;

	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		return &biplanar_yuv_420_layout;

	case DRM_FORMAT_P010:
		return &biplanar_yuv_p010_layout;

	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRA5551:
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_GR88:
	case DRM_FORMAT_RG88:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_MTISP_SXYZW10:
		return &packed_2bpp_layout;

	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
		return &packed_3bpp_layout;

	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_AYUV:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XRGB8888:
		return &packed_4bpp_layout;

	case DRM_FORMAT_ABGR16161616F:
		return &packed_8bpp_layout;

	default:
		drv_log("UNKNOWN FORMAT %d\n", format);
		return NULL;
	}
}

size_t drv_num_planes_from_format(uint32_t format)
{
	const struct planar_layout *layout = layout_from_format(format);

	/*
	 * drv_bo_new calls this function early to query number of planes and
	 * considers 0 planes to mean unknown format, so we have to support
	 * that.  All other layout_from_format() queries can assume that the
	 * format is supported and that the return value is non-NULL.
	 */

	return layout ? layout->num_planes : 0;
}

size_t drv_num_planes_from_modifier(struct driver *drv, uint32_t format, uint64_t modifier)
{
	size_t planes = drv_num_planes_from_format(format);

	/* Disallow unsupported formats. */
	if (!planes)
		return 0;

	if (drv->backend->num_planes_from_modifier && modifier != DRM_FORMAT_MOD_INVALID &&
	    modifier != DRM_FORMAT_MOD_LINEAR)
		return drv->backend->num_planes_from_modifier(drv, format, modifier);

	return planes;
}

uint32_t drv_height_from_format(uint32_t format, uint32_t height, size_t plane)
{
	const struct planar_layout *layout = layout_from_format(format);

	assert(plane < layout->num_planes);

	return DIV_ROUND_UP(height, layout->vertical_subsampling[plane]);
}

uint32_t drv_vertical_subsampling_from_format(uint32_t format, size_t plane)
{
	const struct planar_layout *layout = layout_from_format(format);

	assert(plane < layout->num_planes);

	return layout->vertical_subsampling[plane];
}

uint32_t drv_bytes_per_pixel_from_format(uint32_t format, size_t plane)
{
	const struct planar_layout *layout = layout_from_format(format);

	assert(plane < layout->num_planes);

	return layout->bytes_per_pixel[plane];
}

/*
 * This function returns the stride for a given format, width and plane.
 */
uint32_t drv_stride_from_format(uint32_t format, uint32_t width, size_t plane)
{
	const struct planar_layout *layout = layout_from_format(format);
	assert(plane < layout->num_planes);

	uint32_t plane_width = DIV_ROUND_UP(width, layout->horizontal_subsampling[plane]);
	uint32_t stride = plane_width * layout->bytes_per_pixel[plane];

	/*
	 * The stride of Android YV12 buffers is required to be aligned to 16 bytes
	 * (see <system/graphics.h>).
	 */
	if (format == DRM_FORMAT_YVU420_ANDROID)
		stride = (plane == 0) ? ALIGN(stride, 32) : ALIGN(stride, 16);

	return stride;
}

uint32_t drv_size_from_format(uint32_t format, uint32_t stride, uint32_t height, size_t plane)
{
	return stride * drv_height_from_format(format, height, plane);
}

static uint32_t subsample_stride(uint32_t stride, uint32_t format, size_t plane)
{
	if (plane != 0) {
		switch (format) {
		case DRM_FORMAT_YVU420:
		case DRM_FORMAT_YVU420_ANDROID:
			stride = DIV_ROUND_UP(stride, 2);
			break;
		}
	}

	return stride;
}

/*
 * This function fills in the buffer object given the driver aligned stride of
 * the first plane, height and a format. This function assumes there is just
 * one kernel buffer per buffer object.
 */
int drv_bo_from_format(struct bo *bo, uint32_t stride, uint32_t aligned_height, uint32_t format)
{
	uint32_t padding[DRV_MAX_PLANES] = { 0 };
	return drv_bo_from_format_and_padding(bo, stride, aligned_height, format, padding);
}

int drv_bo_from_format_and_padding(struct bo *bo, uint32_t stride, uint32_t aligned_height,
				   uint32_t format, uint32_t padding[DRV_MAX_PLANES])
{
	size_t p, num_planes;
	uint32_t offset = 0;

	num_planes = drv_num_planes_from_format(format);
	assert(num_planes);

	/*
	 * HAL_PIXEL_FORMAT_YV12 requires that (see <system/graphics.h>):
	 *  - the aligned height is same as the buffer's height.
	 *  - the chroma stride is 16 bytes aligned, i.e., the luma's strides
	 *    is 32 bytes aligned.
	 */
	if (format == DRM_FORMAT_YVU420_ANDROID) {
		assert(aligned_height == bo->meta.height);
		assert(stride == ALIGN(stride, 32));
	}

	for (p = 0; p < num_planes; p++) {
		bo->meta.strides[p] = subsample_stride(stride, format, p);
		bo->meta.sizes[p] =
		    drv_size_from_format(format, bo->meta.strides[p], aligned_height, p) +
		    padding[p];
		bo->meta.offsets[p] = offset;
		offset += bo->meta.sizes[p];
	}

	bo->meta.total_size = offset;
	return 0;
}

int drv_bo_munmap(struct bo *bo, struct vma *vma)
{
	return munmap(vma->addr, vma->length);
}

int drv_mapping_destroy(struct bo *bo)
{
	int ret;
	size_t plane;
	struct mapping *mapping;
	uint32_t idx;

	/*
	 * This function is called right before the buffer is destroyed. It will free any mappings
	 * associated with the buffer.
	 */

	idx = 0;
	for (plane = 0; plane < bo->meta.num_planes; plane++) {
		while (idx < drv_array_size(bo->drv->mappings)) {
			mapping = (struct mapping *)drv_array_at_idx(bo->drv->mappings, idx);
			if (mapping->vma->handle != bo->handles[plane].u32) {
				idx++;
				continue;
			}

			if (!--mapping->vma->refcount) {
				ret = bo->drv->backend->bo_unmap(bo, mapping->vma);
				if (ret) {
					drv_log("munmap failed\n");
					return ret;
				}

				free(mapping->vma);
			}

			/* This shrinks and shifts the array, so don't increment idx. */
			drv_array_remove(bo->drv->mappings, idx);
		}
	}

	return 0;
}

int drv_get_prot(uint32_t map_flags)
{
	return (BO_MAP_WRITE & map_flags) ? PROT_WRITE | PROT_READ : PROT_READ;
}
void drv_add_combination(struct driver *drv, const uint32_t format,
			 struct format_metadata *metadata, uint64_t use_flags)
{
	struct combination combo = { .format = format,
				     .metadata = *metadata,
				     .use_flags = use_flags };

	drv_array_append(drv->combos, &combo);
}

void drv_add_combinations(struct driver *drv, const uint32_t *formats, uint32_t num_formats,
			  struct format_metadata *metadata, uint64_t use_flags)
{
	uint32_t i;

	for (i = 0; i < num_formats; i++) {
		struct combination combo = { .format = formats[i],
					     .metadata = *metadata,
					     .use_flags = use_flags };

		drv_array_append(drv->combos, &combo);
	}
}

void drv_modify_combination(struct driver *drv, uint32_t format, struct format_metadata *metadata,
			    uint64_t use_flags)
{
	uint32_t i;
	struct combination *combo;
	/* Attempts to add the specified flags to an existing combination. */
	for (i = 0; i < drv_array_size(drv->combos); i++) {
		combo = (struct combination *)drv_array_at_idx(drv->combos, i);
		if (combo->format == format && combo->metadata.tiling == metadata->tiling &&
		    combo->metadata.modifier == metadata->modifier)
			combo->use_flags |= use_flags;
	}
}

int drv_modify_linear_combinations(struct driver *drv)
{
	/*
	 * All current drivers can scanout linear XRGB8888/ARGB8888 as a primary
	 * plane and as a cursor.
	 */
	drv_modify_combination(drv, DRM_FORMAT_XRGB8888, &LINEAR_METADATA,
			       BO_USE_CURSOR | BO_USE_SCANOUT);
	drv_modify_combination(drv, DRM_FORMAT_ARGB8888, &LINEAR_METADATA,
			       BO_USE_CURSOR | BO_USE_SCANOUT);
	return 0;
}

/*
 * Pick the best modifier from modifiers, according to the ordering
 * given by modifier_order.
 */
uint64_t drv_pick_modifier(const uint64_t *modifiers, uint32_t count,
			   const uint64_t *modifier_order, uint32_t order_count)
{
	uint32_t i, j;

	for (i = 0; i < order_count; i++) {
		for (j = 0; j < count; j++) {
			if (modifiers[j] == modifier_order[i]) {
				return modifiers[j];
			}
		}
	}

	return DRM_FORMAT_MOD_LINEAR;
}

/*
 * Search a list of modifiers to see if a given modifier is present
 */
bool drv_has_modifier(const uint64_t *list, uint32_t count, uint64_t modifier)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		if (list[i] == modifier)
			return true;

	return false;
}

/*
 * Map internal fourcc codes back to standard fourcc codes.
 */
uint32_t drv_get_standard_fourcc(uint32_t fourcc_internal)
{
	return (fourcc_internal == DRM_FORMAT_YVU420_ANDROID) ? DRM_FORMAT_YVU420 : fourcc_internal;
}

uint32_t drv_resolve_format_helper(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Common camera implementation defined format. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE))
			return DRM_FORMAT_NV12;
		/* A common hack: See b/28671744 */
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		/* Common flexible video format. */
		return DRM_FORMAT_NV12;
	default:
		return format;
	}
}

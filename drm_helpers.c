/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
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
#include <xf86drm.h>

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"


int drv_dumb_bo_create_ex(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
        uint64_t use_flags, uint64_t quirks)
{
  int ret;
  size_t plane;
  uint32_t aligned_width, aligned_height;
  struct drm_mode_create_dumb create_dumb = { 0 };

  aligned_width = width;
  aligned_height = height;
  switch (format) {
  case DRM_FORMAT_R16:
    /* HAL_PIXEL_FORMAT_Y16 requires that the buffer's width be 16 pixel
     * aligned. See hardware/interfaces/graphics/common/1.0/types.hal. */
    aligned_width = ALIGN(width, 16);
    break;
  case DRM_FORMAT_YVU420_ANDROID:
    /* HAL_PIXEL_FORMAT_YV12 requires that the buffer's height not
     * be aligned. Update 'height' so that drv_bo_from_format below
     * uses the non-aligned height. */
    height = bo->meta.height;

    /* Align width to 32 pixels, so chroma strides are 16 bytes as
     * Android requires. */
    aligned_width = ALIGN(width, 32);

    /* Adjust the height to include room for chroma planes. */
    aligned_height = 3 * DIV_ROUND_UP(height, 2);
    break;
  case DRM_FORMAT_YVU420:
  case DRM_FORMAT_NV12:
  case DRM_FORMAT_NV21:
    /* Adjust the height to include room for chroma planes */
    aligned_height = 3 * DIV_ROUND_UP(height, 2);
    break;
  default:
    break;
  }

  if (quirks & BO_QUIRK_DUMB32BPP) {
    aligned_width =
        DIV_ROUND_UP(aligned_width * layout_from_format(format)->bytes_per_pixel[0], 4);
    create_dumb.bpp = 32;
  } else {
    create_dumb.bpp = layout_from_format(format)->bytes_per_pixel[0] * 8;
  }
  create_dumb.width = aligned_width;
  create_dumb.height = aligned_height;
  create_dumb.flags = 0;

  ret = drmIoctl(bo->drv->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
  if (ret) {
    drv_log("DRM_IOCTL_MODE_CREATE_DUMB failed (%d, %d)\n", bo->drv->fd, errno);
    return -errno;
  }

  drv_bo_from_format(bo, create_dumb.pitch, height, format);

  for (plane = 0; plane < bo->meta.num_planes; plane++)
    bo->handles[plane].u32 = create_dumb.handle;

  bo->meta.total_size = create_dumb.size;
  return 0;
}

int drv_dumb_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
           uint64_t use_flags)
{
  return drv_dumb_bo_create_ex(bo, width, height, format, use_flags, BO_QUIRK_NONE);
}

int drv_dumb_bo_destroy(struct bo *bo)
{
  int ret;
  struct drm_mode_destroy_dumb destroy_dumb = { 0 };

  destroy_dumb.handle = bo->handles[0].u32;
  ret = drmIoctl(bo->drv->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
  if (ret) {
    drv_log("DRM_IOCTL_MODE_DESTROY_DUMB failed (handle=%x)\n", bo->handles[0].u32);
    return -errno;
  }

  return 0;
}

int drv_gem_bo_destroy(struct bo *bo)
{
  struct drm_gem_close gem_close;
  int ret, error = 0;
  size_t plane, i;

  for (plane = 0; plane < bo->meta.num_planes; plane++) {
    for (i = 0; i < plane; i++)
      if (bo->handles[i].u32 == bo->handles[plane].u32)
        break;
    /* Make sure close hasn't already been called on this handle */
    if (i != plane)
      continue;

    memset(&gem_close, 0, sizeof(gem_close));
    gem_close.handle = bo->handles[plane].u32;

    ret = drmIoctl(bo->drv->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    if (ret) {
      drv_log("DRM_IOCTL_GEM_CLOSE failed (handle=%x) error %d\n",
        bo->handles[plane].u32, ret);
      error = -errno;
    }
  }

  return error;
}

int drv_prime_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
  int ret;
  size_t plane;
  struct drm_prime_handle prime_handle;

  for (plane = 0; plane < bo->meta.num_planes; plane++) {
    memset(&prime_handle, 0, sizeof(prime_handle));
    prime_handle.fd = data->fds[plane];

    ret = drmIoctl(bo->drv->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle);

    if (ret) {
      drv_log("DRM_IOCTL_PRIME_FD_TO_HANDLE failed (fd=%u)\n", prime_handle.fd);

      /*
       * Need to call GEM close on planes that were opened,
       * if any. Adjust the num_planes variable to be the
       * plane that failed, so GEM close will be called on
       * planes before that plane.
       */
      bo->meta.num_planes = plane;
      drv_gem_bo_destroy(bo);
      return -errno;
    }

    bo->handles[plane].u32 = prime_handle.handle;
  }
  bo->meta.tiling = data->tiling;

  return 0;
}

void *drv_dumb_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
  int ret;
  size_t i;
  struct drm_mode_map_dumb map_dumb;

  memset(&map_dumb, 0, sizeof(map_dumb));
  map_dumb.handle = bo->handles[plane].u32;

  ret = drmIoctl(bo->drv->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
  if (ret) {
    drv_log("DRM_IOCTL_MODE_MAP_DUMB failed\n");
    return MAP_FAILED;
  }

  for (i = 0; i < bo->meta.num_planes; i++)
    if (bo->handles[i].u32 == bo->handles[plane].u32)
      vma->length += bo->meta.sizes[i];

  return mmap(0, vma->length, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
        map_dumb.offset);
}

int drv_bo_munmap(struct bo *bo, struct vma *vma)
{
  return munmap(vma->addr, vma->length);
}

uintptr_t drv_get_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
  void *count;
  uintptr_t num = 0;

  if (!drmHashLookup(drv->buffer_table, bo->handles[plane].u32, &count))
    num = (uintptr_t)(count);

  return num;
}

void drv_increment_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
  uintptr_t num = drv_get_reference_count(drv, bo, plane);

  /* If a value isn't in the table, drmHashDelete is a no-op */
  drmHashDelete(drv->buffer_table, bo->handles[plane].u32);
  drmHashInsert(drv->buffer_table, bo->handles[plane].u32, (void *)(num + 1));
}

void drv_decrement_reference_count(struct driver *drv, struct bo *bo, size_t plane)
{
  uintptr_t num = drv_get_reference_count(drv, bo, plane);

  drmHashDelete(drv->buffer_table, bo->handles[plane].u32);

  if (num > 0)
    drmHashInsert(drv->buffer_table, bo->handles[plane].u32, (void *)(num - 1));
}


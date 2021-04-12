/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gbm.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>

class GbmDevice
{
      public:
	void SetUp()
	{
		fd_ = open("/dev/magma0", O_RDWR | O_CLOEXEC);
		ASSERT_GE(fd_, 0);

		device_ = gbm_create_device(fd_);
		ASSERT_TRUE(device_);
	}

	void TearDown()
	{
		gbm_device_destroy(device_);
		device_ = nullptr;

		close(fd_);
		fd_ = -1;
	}

	struct gbm_device *device()
	{
		return device_;
	}

	int fd_ = -1;
	struct gbm_device *device_ = nullptr;
};

class MagmaGbmTest : public testing::Test
{
      public:
	void SetUp() override
	{
		gbm_.SetUp();
	}

	void TearDown() override
	{
		gbm_.TearDown();
	}

	struct gbm_device *device()
	{
		return gbm_.device();
	}

	GbmDevice gbm_;
};

constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
constexpr uint32_t kDefaultFormat = GBM_FORMAT_ARGB8888;

TEST_F(MagmaGbmTest, CreateLinear)
{
	std::vector<uint64_t> modifiers{ DRM_FORMAT_MOD_LINEAR };
	struct gbm_bo *bo =
	    gbm_bo_create_with_modifiers(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat,
					 modifiers.data(), modifiers.size());
	ASSERT_TRUE(bo);
	EXPECT_EQ(DRM_FORMAT_MOD_LINEAR, gbm_bo_get_modifier(bo));
	gbm_bo_destroy(bo);
}

TEST_F(MagmaGbmTest, CreateIntelX)
{
	std::vector<uint64_t> modifiers{ I915_FORMAT_MOD_X_TILED };
	struct gbm_bo *bo =
	    gbm_bo_create_with_modifiers(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat,
					 modifiers.data(), modifiers.size());
	ASSERT_TRUE(bo);
	EXPECT_EQ(I915_FORMAT_MOD_X_TILED, gbm_bo_get_modifier(bo));
	gbm_bo_destroy(bo);
}

TEST_F(MagmaGbmTest, CreateIntelY)
{
	std::vector<uint64_t> modifiers{ I915_FORMAT_MOD_Y_TILED };
	struct gbm_bo *bo =
	    gbm_bo_create_with_modifiers(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat,
					 modifiers.data(), modifiers.size());
	ASSERT_TRUE(bo);
	EXPECT_EQ(I915_FORMAT_MOD_Y_TILED, gbm_bo_get_modifier(bo));
	gbm_bo_destroy(bo);
}

TEST_F(MagmaGbmTest, CreateIntelBest)
{
	std::vector<uint64_t> modifiers{ DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED,
					 I915_FORMAT_MOD_Y_TILED };
	struct gbm_bo *bo =
	    gbm_bo_create_with_modifiers(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat,
					 modifiers.data(), modifiers.size());
	ASSERT_TRUE(bo);
	EXPECT_EQ(I915_FORMAT_MOD_Y_TILED, gbm_bo_get_modifier(bo));
	gbm_bo_destroy(bo);
}

class MagmaGbmTestWithUsage : public MagmaGbmTest, public testing::WithParamInterface<uint32_t>
{
};

TEST_P(MagmaGbmTestWithUsage, Create)
{
	uint32_t usage = GetParam();

	struct gbm_bo *bo =
	    gbm_bo_create(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat, usage);
	ASSERT_TRUE(bo);

	if (usage & GBM_BO_USE_LINEAR) {
		EXPECT_EQ(DRM_FORMAT_MOD_LINEAR, gbm_bo_get_modifier(bo));
	} else {
		EXPECT_EQ(I915_FORMAT_MOD_Y_TILED, gbm_bo_get_modifier(bo));
	}

	gbm_bo_destroy(bo);
}

TEST_P(MagmaGbmTestWithUsage, Import)
{
	GbmDevice gbm2;
	gbm2.SetUp();

	constexpr uint32_t kPattern = 0xabcd1234;

	struct gbm_bo *bo =
	    gbm_bo_create(device(), kDefaultWidth, kDefaultHeight, kDefaultFormat, GetParam());
	ASSERT_TRUE(bo);

	{
		uint32_t stride;
		void *map_data;
		void *addr = gbm_bo_map(bo, 0, 0, kDefaultWidth, kDefaultHeight,
					GBM_BO_TRANSFER_WRITE, &stride, &map_data);
		ASSERT_NE(addr, MAP_FAILED);
		*reinterpret_cast<uint32_t *>(addr) = kPattern;
		gbm_bo_unmap(bo, map_data);
	}

	struct gbm_import_fd_data import;
	import.fd = gbm_bo_get_fd(bo);
	import.width = gbm_bo_get_width(bo);
	import.height = gbm_bo_get_height(bo);
	import.stride = gbm_bo_get_stride(bo);
	import.format = gbm_bo_get_format(bo);
	EXPECT_GE(import.fd, 0);
	EXPECT_EQ(import.width, kDefaultWidth);
	EXPECT_EQ(import.height, kDefaultHeight);
	EXPECT_EQ(import.format, kDefaultFormat);

	struct gbm_bo *bo2 = gbm_bo_import(gbm2.device(), GBM_BO_IMPORT_FD, &import, GetParam());
	ASSERT_TRUE(bo2);

	{
		uint32_t stride;
		void *map_data;
		void *addr = gbm_bo_map(bo2, 0, 0, kDefaultWidth, kDefaultHeight,
					GBM_BO_TRANSFER_READ, &stride, &map_data);
		ASSERT_NE(addr, MAP_FAILED);
		EXPECT_EQ(*reinterpret_cast<uint32_t *>(addr), kPattern);
		gbm_bo_unmap(bo, map_data);
	}

	gbm_bo_destroy(bo);
	gbm_bo_destroy(bo2);

	gbm2.TearDown();
}

INSTANTIATE_TEST_SUITE_P(MagmaGbmTestWithUsage, MagmaGbmTestWithUsage,
			 ::testing::Values(GBM_BO_USE_RENDERING,
					   GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
					   GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT,
					   GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR |
					       GBM_BO_USE_SCANOUT,
					   GBM_BO_USE_LINEAR));

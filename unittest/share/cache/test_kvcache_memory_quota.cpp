/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gtest/gtest.h"
#include "lib/ob_errno.h"
#include "share/cache/ob_kvcache_store.h"
#include "share/cache/ob_kvcache_hazard_domain.h"

using oceanbase::common::MemoryQuotaSample;
using oceanbase::common::HazardDomain;
using oceanbase::common::HazptrHolder;
using oceanbase::common::ObKVCacheStore;
using oceanbase::common::ObKVMemBlockHandle;
using oceanbase::common::OB_ALLOCATE_MEMORY_FAILED;
using oceanbase::common::OB_SUCCESS;

TEST(TestKVCacheMemoryQuota, fixed_block_admission)
{
  EXPECT_EQ(0,
            oceanbase::common::ObKVCacheStore::compute_fixed_cache_limit(63,
                                                                          64));
  EXPECT_EQ(192,
            oceanbase::common::ObKVCacheStore::compute_fixed_cache_limit(255,
                                                                          64));
  EXPECT_TRUE(
      oceanbase::common::ObKVCacheStore::can_reserve_cache_size(128, 64, 192));
  EXPECT_FALSE(
      oceanbase::common::ObKVCacheStore::can_reserve_cache_size(192, 64, 192));
}

TEST(TestKVCacheMemoryQuota, real_store_pressure_shrink_free_and_destroy)
{
  static const int64_t BLOCK_SIZE = 64L << 10;
  static const int64_t MAX_CACHE_SIZE = 8 * BLOCK_SIZE;
  ObKVCacheStore store;
  ASSERT_EQ(OB_SUCCESS, store.init(MAX_CACHE_SIZE, BLOCK_SIZE,
                                  4 * BLOCK_SIZE));

  ObKVMemBlockHandle *first = nullptr;
  ObKVMemBlockHandle *second = nullptr;
  ASSERT_EQ(OB_SUCCESS, store.alloc_mbhandle(first));
  ASSERT_NE(nullptr, first);
  ASSERT_EQ(OB_SUCCESS, store.alloc_mbhandle(second));
  ASSERT_NE(nullptr, second);

  const MemoryQuotaSample allocated = store.get_memory_quota_sample();
  const int64_t first_charge = ob_malloc_usable_size(first->mem_block_);
  const int64_t second_charge = ob_malloc_usable_size(second->mem_block_);
  EXPECT_GT(allocated.committed_bytes_, 0);
  EXPECT_EQ(first_charge + second_charge, allocated.committed_bytes_);
  EXPECT_EQ(0, allocated.reserved_bytes_);
  EXPECT_LE(allocated.committed_bytes_, allocated.limit_bytes_);

  // Retire one real store block, then shrink below the current committed
  // charge.  Fill the shrunken quota, then force the synchronous wash/retry
  // pressure path.  The retired handle is still protected by the current
  // qclock generation, so this request is deterministically rejected.
  ASSERT_EQ(OB_SUCCESS, store.free(first));
  ASSERT_EQ(OB_SUCCESS, store.set_cache_memory_limit(3 * BLOCK_SIZE));
  ObKVMemBlockHandle *after_shrink = nullptr;
  ASSERT_EQ(OB_SUCCESS, store.alloc_mbhandle(after_shrink));
  ObKVMemBlockHandle *rejected = nullptr;
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, store.alloc_mbhandle(rejected));
  EXPECT_EQ(nullptr, rejected);
  const MemoryQuotaSample pressured = store.get_memory_quota_sample();
  EXPECT_EQ(0, pressured.reserved_bytes_);
  EXPECT_GT(pressured.reject_count_, 0);

  ASSERT_EQ(OB_SUCCESS, store.free_mbhandle(second, false));
  ASSERT_EQ(OB_SUCCESS, store.free_mbhandle(after_shrink, false));
  const MemoryQuotaSample freed = store.get_memory_quota_sample();
  EXPECT_LT(freed.committed_bytes_, allocated.committed_bytes_);

  store.destroy();
  const MemoryQuotaSample destroyed = store.get_memory_quota_sample();
  EXPECT_EQ(0, destroyed.limit_bytes_);
  EXPECT_EQ(0, destroyed.committed_bytes_);
  EXPECT_EQ(0, destroyed.reserved_bytes_);
}

TEST(TestKVCacheMemoryQuota, destroy_drains_hazard_protected_retired_block)
{
  static const int64_t BLOCK_SIZE = 64L << 10;
  static const int64_t MAX_CACHE_SIZE = 8 * BLOCK_SIZE;

  for (int64_t round = 0; round < 2; ++round) {
    ObKVCacheStore store;
    ASSERT_EQ(OB_SUCCESS, store.init(MAX_CACHE_SIZE, BLOCK_SIZE,
                                    4 * BLOCK_SIZE));

    ObKVMemBlockHandle *handle = nullptr;
    ASSERT_EQ(OB_SUCCESS, store.alloc_mbhandle(handle));
    ASSERT_NE(nullptr, handle);
    const int64_t committed = store.get_memory_quota_sample().committed_bytes_;
    ASSERT_GT(committed, 0);

    HazptrHolder holder;
    bool protected_successfully = false;
    ASSERT_EQ(OB_SUCCESS, holder.protect(protected_successfully, handle));
    ASSERT_TRUE(protected_successfully);
    ASSERT_EQ(OB_SUCCESS, store.free(handle));
    EXPECT_FALSE(store.wash());
    EXPECT_EQ(committed, store.get_memory_quota_sample().committed_bytes_);
    EXPECT_GT(HazardDomain::get_instance().get_retired_memory_size(), 0);

    holder.release();
    store.destroy();
    EXPECT_EQ(0, HazardDomain::get_instance().get_retired_memory_size());
    const MemoryQuotaSample destroyed = store.get_memory_quota_sample();
    EXPECT_EQ(0, destroyed.limit_bytes_);
    EXPECT_EQ(0, destroyed.committed_bytes_);
    EXPECT_EQ(0, destroyed.reserved_bytes_);
  }
}

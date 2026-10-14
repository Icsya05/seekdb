/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include "gtest/gtest.h"
#include "lib/alloc/alloc_func.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/basic/ob_chunk_row_store.h"
#include "sql/engine/basic/ob_ra_datum_store.h"
#include "sql/engine/basic/ob_workarea_memory_limit.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "sql/dtl/ob_dtl_interm_result_manager.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/tmp_file/ob_tmp_file_manager.h"

#include <vector>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

MemoryQuota *test_workarea_quota = nullptr;

class TestTmpFileManagerGuard final
{
public:
  TestTmpFileManagerGuard() : started_(false), bound_(false) {}

  int init()
  {
    int ret = manager_.init();
    if (OB_SUCCESS == ret) {
      ret = manager_.start();
      started_ = OB_SUCCESS == ret;
    }
    if (OB_SUCCESS == ret) {
      oceanbase::share::bind_server_service<oceanbase::tmp_file::ObTmpFileManager>(
          &manager_);
      bound_ = true;
    }
    return ret;
  }

  ~TestTmpFileManagerGuard()
  {
    if (bound_) {
      oceanbase::share::unbind_server_service<oceanbase::tmp_file::ObTmpFileManager>();
    }
    if (started_) {
      manager_.stop();
      manager_.wait();
    }
    manager_.destroy();
  }

private:
  oceanbase::tmp_file::ObTmpFileManager manager_;
  bool started_;
  bool bound_;
};

MemoryQuota *resolve_test_workarea_quota(const int64_t ctx_id)
{
  return ObCtxIds::WORK_AREA == ctx_id ? test_workarea_quota : nullptr;
}

class TestWorkareaQuotaResolverGuard final
{
public:
  explicit TestWorkareaQuotaResolverGuard(MemoryQuota &quota)
  {
    test_workarea_quota = &quota;
    set_memory_quota_resolver(ObCtxIds::WORK_AREA,
                              resolve_test_workarea_quota);
  }
  ~TestWorkareaQuotaResolverGuard()
  {
    set_memory_quota_resolver(ObCtxIds::WORK_AREA, nullptr);
    test_workarea_quota = nullptr;
  }
};

class TestSqlMemoryManagerGuard final
{
public:
  TestSqlMemoryManagerGuard() : manager_(nullptr), bound_(false) {}

  int init()
  {
    int ret = ObSqlMemoryManager::server_module_new(manager_);
    if (OB_SUCCESS == ret) {
      ret = ObSqlMemoryManager::server_module_init(manager_);
    }
    if (OB_SUCCESS == ret) {
      oceanbase::share::bind_server_service<ObSqlMemoryManager>(manager_);
      bound_ = true;
    }
    return ret;
  }

  ~TestSqlMemoryManagerGuard()
  {
    if (bound_) {
      oceanbase::share::unbind_server_service<ObSqlMemoryManager>();
    }
    ObSqlMemoryManager::server_module_destroy(manager_);
  }

  ObSqlMemoryManager &get() { return *manager_; }

private:
  ObSqlMemoryManager *manager_;
  bool bound_;
};

class TestStoredRow final
{
public:
  explicit TestStoredRow(const int64_t row_size)
      : bytes_(row_size, 0),
        row_(new (bytes_.data()) ObChunkDatumStore::StoredRow())
  {
    row_->cnt_ = 0;
    row_->row_size_ = static_cast<uint32_t>(row_size);
  }

  const ObChunkDatumStore::StoredRow &get() const { return *row_; }

private:
  std::vector<char> bytes_;
  ObChunkDatumStore::StoredRow *row_;
};

// Keep the quota/spill policy test inside the SQL module.  The temporary-file
// service has its own module tests; here a successful write is enough to drive
// the real ObChunkDatumStore dump, block reuse and reclaim accounting paths.
class TestChunkDatumStore final : public ObChunkDatumStore
{
public:
  TestChunkDatumStore(const oceanbase::lib::ObLabel &label,
                      ObIAllocator &allocator,
                      ObSqlMemoryCallback &callback)
      : ObChunkDatumStore(label, &allocator),
        callback_(callback),
        spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, const int64_t size) override
  {
    int ret = OB_SUCCESS;
    if (size < 0 || (size > 0 && nullptr == buf)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (size > 0) {
      spilled_bytes_ += size;
      callback_.dumped(size);
    }
    return ret;
  }

  ObSqlMemoryCallback &callback_;
  int64_t spilled_bytes_;
};

class DefaultBackedChunkDatumStore final : public ObChunkDatumStore
{
public:
  explicit DefaultBackedChunkDatumStore(const oceanbase::lib::ObLabel &label)
      : ObChunkDatumStore(label), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, const int64_t size) override
  {
    int ret = OB_SUCCESS;
    if (size < 0 || (size > 0 && nullptr == buf)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

// Changes the shared limit immediately before one backing allocation.  This
// deterministically models a concurrent store or dynamic limit shrink after a
// store's spill sample but before hard-quota admission.
class ShrinkQuotaOnNextAllocation final : public ObIAllocator
{
public:
  ShrinkQuotaOnNextAllocation(ObIAllocator &allocator, MemoryQuota &quota)
      : allocator_(allocator), quota_(quota), reclaim_margin_(0), armed_(false),
        triggered_(false)
  {}

  void arm(const int64_t reclaim_margin)
  {
    reclaim_margin_ = reclaim_margin;
    armed_ = true;
    triggered_ = false;
  }

  bool triggered() const { return triggered_; }

  void *alloc(const int64_t size) override
  {
    shrink_if_armed(size);
    return allocator_.alloc(size);
  }

  void *alloc(const int64_t size, const ObMemAttr &attr) override
  {
    shrink_if_armed(size);
    return allocator_.alloc(size, attr);
  }

  void free(void *ptr) override { allocator_.free(ptr); }
  int64_t total() const override { return allocator_.total(); }
  int64_t used() const override { return allocator_.used(); }

private:
  void shrink_if_armed(const int64_t size)
  {
    if (armed_) {
      armed_ = false;
      triggered_ = true;
      const int64_t requested_charge =
          size + TrackedAllocator::header_size();
      quota_.set_limit(quota_.committed() + requested_charge
                       - reclaim_margin_);
    }
  }

  ObIAllocator &allocator_;
  MemoryQuota &quota_;
  int64_t reclaim_margin_;
  bool armed_;
  bool triggered_;
};

std::string batch_value(const int64_t index)
{
  return "datum-" + std::to_string(index) + "-payload";
}

int add_string_vectorized_batch(ObChunkDatumStore &store,
                                ObIAllocator &allocator,
                                const int64_t batch_size,
                                std::vector<ObChunkDatumStore::StoredRow *> &rows)
{
  int ret = OB_SUCCESS;
  ObExecContext exec_ctx(allocator);
  ObEvalCtx eval_ctx(exec_ctx);
  eval_ctx.set_max_batch_size(batch_size);

  ObExpr expr;
  expr.datum_meta_.type_ = ObVarcharType;
  expr.batch_result_ = true;
  expr.frame_idx_ = 0;
  expr.datum_off_ = 0;
  expr.eval_info_off_ = sizeof(ObDatum) * batch_size;
  const int64_t frame_size = expr.eval_info_off_ + sizeof(ObEvalInfo);
  std::vector<uint64_t> frame_words(
      (frame_size + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
  char *frame = reinterpret_cast<char *>(frame_words.data());
  std::vector<std::string> payloads(batch_size);
  for (int64_t i = 0; i < batch_size; ++i) {
    payloads[i] = batch_value(i);
    ObDatum *datum = new (frame + sizeof(ObDatum) * i) ObDatum();
    datum->set_string(payloads[i].data(), payloads[i].size());
  }
  ObEvalInfo *eval_info = new (frame + expr.eval_info_off_) ObEvalInfo();
  eval_info->projected_ = true;
  char *frames[] = {frame};
  exec_ctx.set_frames(frames);
  eval_ctx.frames_ = frames;
  ObSEArray<ObExpr *, 1> exprs;
  if (OB_FAIL(exprs.push_back(&expr))) {
    return ret;
  }
  std::vector<char> skip_mem(ObBitVector::memory_size(batch_size), 0);
  ObBitVector *skip = to_bit_vector(skip_mem.data());
  skip->reset(batch_size);
  std::vector<uint16_t> selector(batch_size);
  rows.resize(batch_size, nullptr);
  for (int64_t i = 0; i < batch_size; ++i) {
    selector[i] = static_cast<uint16_t>(i);
  }
  ret = store.add_batch(exprs, eval_ctx, *skip, batch_size,
                        selector.data(), batch_size, rows.data());
  return ret;
}

class TestRADatumStore final : public ObRADatumStore
{
public:
  TestRADatumStore() : ObRADatumStore(), spilled_bytes_(0) {}

  TestRADatumStore(ObIAllocator &allocator, ObSqlMemoryCallback &)
      : ObRADatumStore(&allocator), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(BlockIndex &bi, void *buf, int64_t size) override
  {
    const int ret = ObRADatumStore::write_file(bi, buf, size);
    if (OB_SUCCESS == ret) {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

class TestChunkRowStore final : public ObChunkRowStore
{
public:
  TestChunkRowStore(ObIAllocator &allocator, ObSqlMemoryCallback &)
      : ObChunkRowStore(&allocator), spilled_bytes_(0)
  {}

  int64_t spilled_bytes() const { return spilled_bytes_; }

private:
  int write_file(void *buf, int64_t size) override
  {
    const int ret = ObChunkRowStore::write_file(buf, size);
    if (OB_SUCCESS == ret) {
      spilled_bytes_ += size;
    }
    return ret;
  }

  int64_t spilled_bytes_;
};

} // namespace

TEST(TestWorkareaMemoryQuota, zero_inherits_eighty_percent)
{
  EXPECT_EQ(80, calculate_workarea_memory_limit(0, 100));
  EXPECT_EQ(81, calculate_workarea_memory_limit(0, 102));
  EXPECT_EQ(0, calculate_workarea_memory_limit(0, 0));
}

TEST(TestWorkareaMemoryQuota, explicit_limit_is_preserved)
{
  EXPECT_EQ(123, calculate_workarea_memory_limit(123, 1000));
  EXPECT_EQ(UNLIMITED_WORKAREA_MEMORY,
            calculate_workarea_memory_limit(UNLIMITED_WORKAREA_MEMORY, 1000));
}

TEST(TestWorkareaMemoryQuota, manual_startup_establishes_hard_quota)
{
  TestSqlMemoryManagerGuard manager_guard;
  ASSERT_EQ(OB_SUCCESS, manager_guard.init());
  ObSqlMemoryManager &manager = manager_guard.get();
  const int64_t hard_limit = 8L << 20;

  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(
      false, hard_limit, nullptr, false));
  EXPECT_FALSE(manager.enable_auto_memory_mgr());
  EXPECT_EQ(hard_limit, manager.get_max_workarea_size());
  EXPECT_EQ(hard_limit, manager.get_memory_quota_sample().limit_bytes_);
  EXPECT_EQ(hard_limit * 80 / 100,
            calculate_workarea_memory_limit(
                0, manager.get_memory_quota_sample().limit_bytes_));

  MemoryQuota &quota = manager.get_workarea_quota();
  EXPECT_TRUE(quota.reserve(hard_limit));
  EXPECT_FALSE(quota.reserve(1));
  quota.rollback(hard_limit);
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota, auto_manual_transitions_refresh_hard_quota)
{
  TestSqlMemoryManagerGuard manager_guard;
  ASSERT_EQ(OB_SUCCESS, manager_guard.init());
  ObSqlMemoryManager &manager = manager_guard.get();
  const int64_t initial_manual_limit = 16L << 20;
  const int64_t auto_limit = 12L << 20;
  const int64_t final_manual_limit = 4L << 20;

  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(
      false, initial_manual_limit, nullptr, false));
  EXPECT_EQ(initial_manual_limit,
            manager.get_memory_quota_sample().limit_bytes_);

  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(
      true, auto_limit, nullptr, false));
  EXPECT_TRUE(manager.enable_auto_memory_mgr());
  EXPECT_EQ(auto_limit, manager.get_max_workarea_size());
  EXPECT_EQ(auto_limit, manager.get_memory_quota_sample().limit_bytes_);
  EXPECT_EQ(auto_limit * 80 / 100,
            calculate_workarea_memory_limit(
                0, manager.get_memory_quota_sample().limit_bytes_));

  ASSERT_EQ(OB_SUCCESS, manager.calculate_global_bound_size(
      false, final_manual_limit, nullptr, false));
  EXPECT_FALSE(manager.enable_auto_memory_mgr());
  EXPECT_EQ(final_manual_limit, manager.get_max_workarea_size());
  EXPECT_EQ(final_manual_limit,
            manager.get_memory_quota_sample().limit_bytes_);
  EXPECT_EQ(final_manual_limit * 80 / 100,
            calculate_workarea_memory_limit(
                0, manager.get_memory_quota_sample().limit_bytes_));

  MemoryQuota &quota = manager.get_workarea_quota();
  EXPECT_TRUE(quota.reserve(final_manual_limit));
  EXPECT_FALSE(quota.reserve(1));
  quota.rollback(final_manual_limit);
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota, successful_spill_write_records_one_reclaim)
{
  oceanbase::common::MemoryQuota quota(1024);
  ObSqlMemoryTracker tracker;
  tracker.set_memory_quota(&quota);

  tracker.dumped(256);
  EXPECT_EQ(1, quota.reclaim_count());
  EXPECT_EQ(256, tracker.get_total_dump_size());

  tracker.dumped(-256);
  EXPECT_EQ(1, quota.reclaim_count());
  EXPECT_EQ(0, tracker.get_total_dump_size());
}

TEST(TestWorkareaMemoryQuota,
     default_backed_store_enforces_and_releases_component_quota)
{
  TestSqlMemoryManagerGuard manager_guard;
  ASSERT_EQ(OB_SUCCESS, manager_guard.init());
  ObSqlMemoryManager &manager = manager_guard.get();
  MemoryQuota &quota = manager.get_workarea_quota();
  const TestStoredRow row(3L << 10);

  quota.set_limit(1L << 20);
  {
    DefaultBackedChunkDatumStore store("WADefaultStore");
    ASSERT_EQ(OB_SUCCESS,
              store.init(0, ObCtxIds::WORK_AREA, "WADefaultStore",
                         true, 0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    ASSERT_EQ(OB_SUCCESS, store.add_row(row.get()));
    const MemoryQuotaSample allocated = manager.get_memory_quota_sample();
    ASSERT_GT(allocated.committed_bytes_, 0);
    EXPECT_EQ(0, allocated.reserved_bytes_);

    quota.set_limit(allocated.committed_bytes_ +
                    allocated.committed_bytes_ / 4);
    ASSERT_TRUE(should_spill_workarea(
        0, store.get_mem_used(), row.get().row_size_));
    ASSERT_EQ(OB_SUCCESS, store.add_row(row.get()));
    EXPECT_GT(store.spilled_bytes(), 0);
    EXPECT_LE(quota.committed(), quota.limit());
  }
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());

  quota.set_limit(1);
  const int64_t rejected = quota.reject_count();
  {
    ObChunkDatumStore store("WADefaultReject");
    ASSERT_EQ(OB_SUCCESS,
              store.init(0, ObCtxIds::WORK_AREA, "WADefaultReject",
                         false, 0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, store.add_row(row.get()));
    EXPECT_GT(quota.reject_count(), rejected);
  }
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     dtl_default_store_remains_accounted_and_readable_after_handoff)
{
  TestSqlMemoryManagerGuard manager_guard;
  ASSERT_EQ(OB_SUCCESS, manager_guard.init());
  ObSqlMemoryManager &sql_manager = manager_guard.get();
  MemoryQuota &quota = sql_manager.get_workarea_quota();
  quota.set_limit(1L << 20);

  TestTmpFileManagerGuard tmp_file_manager_guard;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager_guard.init());
  oceanbase::sql::dtl::ObDTLIntermResultManager dtl_manager;
  const double saved_max_cpu = oceanbase::share::server_runtime()->max_cpu();
  oceanbase::share::server_runtime()->set_max_cpu(1);
  const int init_ret = dtl_manager.init();
  oceanbase::share::server_runtime()->set_max_cpu(saved_max_cpu);
  ASSERT_EQ(OB_SUCCESS, init_ret);
  oceanbase::sql::dtl::ObDTLIntermResultInfoGuard producer_guard;
  ObMemAttr attr("DtlQuotaTest", ObCtxIds::WORK_AREA);
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.create_interm_result_info(
                attr, producer_guard,
                oceanbase::sql::dtl::ObDTLIntermResultMonitorInfo()));
  ASSERT_NE(nullptr, producer_guard.result_info_);
  oceanbase::sql::dtl::ObDTLIntermResultInfo *result_info =
      producer_guard.result_info_;
  ASSERT_EQ(OB_SUCCESS,
            result_info->datum_store_->init(
                0, ObCtxIds::WORK_AREA, "DtlQuotaTest", false, 0,
                ObChunkDatumStore::MIN_BLOCK_SIZE));
  const TestStoredRow row(1L << 10);
  ASSERT_EQ(OB_SUCCESS, result_info->datum_store_->add_row(row.get()));
  result_info->set_eof(true);
  ASSERT_GT(sql_manager.get_memory_quota_sample().committed_bytes_, 0);

  oceanbase::sql::dtl::ObDTLIntermResultKey key;
  key.channel_id_ = 1;
  key.batch_id_ = 1;
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.insert_interm_result_info(key, result_info));
  producer_guard.reset();

  oceanbase::sql::dtl::ObDTLIntermResultInfoGuard consumer_guard;
  ASSERT_EQ(OB_SUCCESS,
            dtl_manager.atomic_get_interm_result_info(key, consumer_guard));
  ASSERT_NE(nullptr, consumer_guard.result_info_);
  ObChunkDatumStore::Iterator iterator;
  ASSERT_EQ(OB_SUCCESS,
            consumer_guard.result_info_->datum_store_->begin(iterator));
  const ObChunkDatumStore::StoredRow *stored_row = nullptr;
  ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(stored_row));
  ASSERT_NE(nullptr, stored_row);
  EXPECT_EQ(row.get().row_size_, stored_row->row_size_);
  EXPECT_EQ(OB_ITER_END, iterator.get_next_row(stored_row));

  ASSERT_EQ(OB_SUCCESS, dtl_manager.erase_interm_result_info(key));
  consumer_guard.reset();
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     two_zero_limit_stores_spill_before_shared_quota_rejection)
{
  MemoryQuota quota;
  TestWorkareaQuotaResolverGuard quota_resolver_guard(quota);
  ObSqlMemoryTracker tracker;
  tracker.set_memory_quota(&quota);
  DefaultPageAllocator backing_allocator(
      ObMemAttr("WAQuotaAlloc", ObCtxIds::WORK_AREA));
  TrackedAllocator quota_allocator(
      backing_allocator, nullptr, &quota,
      ObMemAttr("WAQuotaAlloc", ObCtxIds::WORK_AREA));
  const TestStoredRow row(3L << 10);

  {
    TestChunkDatumStore first_store(
        "WAQuotaFirst", quota_allocator, tracker);
    TestChunkDatumStore second_store(
        "WAQuotaSecond", quota_allocator, tracker);
    ASSERT_EQ(OB_SUCCESS,
              first_store.init(0, ObCtxIds::WORK_AREA,
                               "WAQuotaFirst", true, 0,
                               ObChunkDatumStore::MIN_BLOCK_SIZE));
    ASSERT_EQ(OB_SUCCESS,
              second_store.init(0, ObCtxIds::WORK_AREA,
                                "WAQuotaSecond", true, 0,
                                ObChunkDatumStore::MIN_BLOCK_SIZE));
    first_store.set_callback(&tracker);
    second_store.set_callback(&tracker);

    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row.get()));
    ASSERT_EQ(OB_SUCCESS, second_store.add_row(row.get()));
    const int64_t aggregate_bytes = quota.committed();
    ASSERT_GT(aggregate_bytes, 0);

    // Both stores remain well below their former per-store threshold, while
    // their combined charge reaches the shared 80% spill watermark.  The
    // remaining hard-quota headroom is deliberately smaller than a new block.
    const int64_t shared_limit = aggregate_bytes + aggregate_bytes / 4;
    quota.set_limit(shared_limit);
    const int64_t spill_watermark =
        calculate_workarea_memory_limit(0, shared_limit);
    ASSERT_LT(first_store.get_mem_used(), spill_watermark);
    ASSERT_LT(second_store.get_mem_used(), spill_watermark);
    ASSERT_TRUE(should_spill_workarea(0, first_store.get_mem_used(),
                                      row.get().row_size_));
    ASSERT_LT(shared_limit - quota.committed(), aggregate_bytes / 2);

    const int64_t reject_count = quota.reject_count();
    const int64_t reclaim_count = quota.reclaim_count();
    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row.get()));
    EXPECT_EQ(2, first_store.get_row_cnt());
    EXPECT_EQ(1, second_store.get_row_cnt());
    EXPECT_EQ(reject_count, quota.reject_count());
    EXPECT_GT(quota.reclaim_count(), reclaim_count);
    EXPECT_GT(first_store.spilled_bytes(), 0);
    EXPECT_LE(quota.committed(), shared_limit);
    ASSERT_EQ(OB_SUCCESS, first_store.finish_add_row(false));
  }

  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     vectorized_batch_suspends_dump_across_block_switch_and_quota_reject)
{
  MemoryQuota quota;
  TestWorkareaQuotaResolverGuard quota_resolver_guard(quota);
  DefaultPageAllocator backing_allocator(
      ObMemAttr("WABatchBacking", ObCtxIds::WORK_AREA));
  ShrinkQuotaOnNextAllocation shrinking_allocator(backing_allocator, quota);
  TrackedAllocator quota_allocator(
      shrinking_allocator, nullptr, &quota,
      ObMemAttr("WABatchQuota", ObCtxIds::WORK_AREA));
  const int64_t batch_size = 1024;

  quota.set_limit(1L << 20);
  {
    DefaultBackedChunkDatumStore store("WABatchOK");
    ASSERT_EQ(OB_SUCCESS,
              store.init(0, ObCtxIds::WORK_AREA, "WABatchOK",
                         true, 0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    std::vector<ObChunkDatumStore::StoredRow *> rows;
    ASSERT_EQ(OB_SUCCESS,
              add_string_vectorized_batch(store, backing_allocator,
                                          batch_size, rows));
    ASSERT_GT(store.get_block_cnt(), 1);
    for (int64_t i = 0; i < batch_size; ++i) {
      ASSERT_NE(nullptr, rows[i]);
      EXPECT_EQ(1, rows[i]->cnt_);
      const std::string expected = batch_value(i);
      EXPECT_EQ(ObString(expected.length(), expected.data()),
                rows[i]->cells()[0].get_string());
    }
    ObChunkDatumStore::Iterator iterator;
    ASSERT_EQ(OB_SUCCESS, store.begin(iterator));
    const ObChunkDatumStore::StoredRow *row = nullptr;
    for (int64_t i = 0; i < batch_size; ++i) {
      ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(row));
      EXPECT_EQ(rows[i], row);
      EXPECT_EQ(1, row->cnt_);
      const std::string expected = batch_value(i);
      EXPECT_EQ(ObString(expected.length(), expected.data()),
                row->cells()[0].get_string());
    }
    EXPECT_EQ(OB_ITER_END, iterator.get_next_row(row));
  }

  quota.set_limit(1L << 20);
  {
    ObSqlMemoryTracker tracker;
    tracker.set_memory_quota(&quota);
    TestChunkDatumStore store("WABatchReject", quota_allocator, tracker);
    ASSERT_EQ(OB_SUCCESS,
              store.init(0, ObCtxIds::WORK_AREA, "WABatchReject",
                         true, 0, ObChunkDatumStore::MIN_BLOCK_SIZE));
    store.set_callback(&tracker);
    std::vector<ObChunkDatumStore::StoredRow *> seed_rows;
    ASSERT_EQ(OB_SUCCESS,
              add_string_vectorized_batch(store, backing_allocator, 1,
                                          seed_rows));
    ASSERT_EQ(1, seed_rows.size());
    ASSERT_NE(nullptr, seed_rows[0]);
    ObChunkDatumStore::StoredRow *seed_row = seed_rows[0];
    const uint32_t seed_size = seed_row->row_size_;
    const std::string seed_value = batch_value(0);
    const int64_t reject_count = quota.reject_count();

    shrinking_allocator.arm(1);
    std::vector<ObChunkDatumStore::StoredRow *> rejected_rows;
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
              add_string_vectorized_batch(store, backing_allocator,
                                          batch_size, rejected_rows));
    EXPECT_TRUE(shrinking_allocator.triggered());
    EXPECT_EQ(0, store.spilled_bytes());
    EXPECT_GT(quota.reject_count(), reject_count);
    ASSERT_NE(nullptr, seed_row);
    EXPECT_EQ(1, seed_row->cnt_);
    EXPECT_EQ(seed_size, seed_row->row_size_);
    EXPECT_EQ(ObString(seed_value.length(), seed_value.data()),
              seed_row->cells()[0].get_string());
  }
  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     default_backed_ra_datum_store_tracks_rejects_spill_and_release)
{
  TestTmpFileManagerGuard tmp_file_manager;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager.init());
  MemoryQuota quota;
  TestWorkareaQuotaResolverGuard quota_resolver_guard(quota);
  std::vector<char> first_payload(40L << 10, 'a');
  std::vector<char> second_payload(40L << 10, 'b');
  ObDatum datum;
  datum.set_string(first_payload.data(), first_payload.size());
  ObSEArray<ObDatum, 1> row;
  ASSERT_EQ(OB_SUCCESS, row.push_back(datum));

  quota.set_limit(1);
  {
    TestRADatumStore store;
    ASSERT_EQ(OB_SUCCESS,
              store.init(ObRADatumStore::BIG_BLOCK_SIZE / 2,
                         ObCtxIds::WORK_AREA, "DefaultWARADatum"));
    const int64_t reject_count = quota.reject_count();
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, store.add_row(row));
    EXPECT_GT(quota.reject_count(), reject_count);
    EXPECT_EQ(0, quota.committed());
    EXPECT_EQ(0, quota.reserved());

    quota.set_limit(4 * ObRADatumStore::BIG_BLOCK_SIZE);
    ASSERT_EQ(OB_SUCCESS, store.add_row(row));
    ASSERT_GT(quota.committed(), 0);

    // FULL_INPUT_SAMPLE appends every input row to this same default-backed
    // store.  Keep adding under a low shared quota to exercise spill and
    // readback without injecting an allocator that would mask the resolver.
    row.at(0).set_string(second_payload.data(), second_payload.size());
    for (int64_t i = 1; i < 32; ++i) {
      ASSERT_EQ(OB_SUCCESS, store.add_row(row));
    }
    EXPECT_GT(store.spilled_bytes(), 0);
    EXPECT_LE(quota.committed(), quota.limit());
    ASSERT_EQ(OB_SUCCESS, store.finish_add_row());

    quota.set_limit(quota.committed() + 2 * ObRADatumStore::BIG_BLOCK_SIZE);
    ObRADatumStore::Reader reader(store);
    const ObRADatumStore::StoredRow *stored_row = nullptr;
    ASSERT_EQ(OB_SUCCESS, reader.get_row(0, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(first_payload.size(), first_payload.data()),
              stored_row->cells()[0].get_string());
    ASSERT_EQ(OB_SUCCESS, reader.get_row(31, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(second_payload.size(), second_payload.data()),
              stored_row->cells()[0].get_string());
  }

  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     two_positive_limit_ra_stores_reclaim_after_shared_quota_admission_race)
{
  TestTmpFileManagerGuard tmp_file_manager;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager.init());
  MemoryQuota quota;
  TestWorkareaQuotaResolverGuard quota_resolver_guard(quota);
  ObSqlMemoryTracker tracker;
  tracker.set_memory_quota(&quota);
  DefaultPageAllocator backing_allocator(
      ObMemAttr("WARADatumAlloc", ObCtxIds::WORK_AREA));
  TrackedAllocator quota_allocator(
      backing_allocator, nullptr, &quota,
      ObMemAttr("WARADatumAlloc", ObCtxIds::WORK_AREA));
  ShrinkQuotaOnNextAllocation race_allocator(quota_allocator, quota);
  std::vector<char> first_payload(40L << 10, 'a');
  std::vector<char> second_payload(40L << 10, 'b');
  ObDatum datum;
  datum.set_string(first_payload.data(), first_payload.size());
  ObSEArray<ObDatum, 1> row;
  ASSERT_EQ(OB_SUCCESS, row.push_back(datum));

  {
    TestRADatumStore first_store(race_allocator, tracker);
    TestRADatumStore second_store(race_allocator, tracker);
    const int64_t local_limit = 16 * ObRADatumStore::BIG_BLOCK_SIZE;
    ASSERT_EQ(OB_SUCCESS,
              first_store.init(local_limit, ObCtxIds::WORK_AREA,
                               "WARADatumFirst"));
    ASSERT_EQ(OB_SUCCESS,
              second_store.init(local_limit, ObCtxIds::WORK_AREA,
                                "WARADatumSecond"));
    first_store.set_mem_stat(&tracker);
    second_store.set_mem_stat(&tracker);

    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    ASSERT_EQ(OB_SUCCESS, second_store.add_row(row));
    const int64_t aggregate_bytes = quota.committed();
    ASSERT_GT(aggregate_bytes, 0);

    // The high limit keeps the pre-allocation sample below its spill
    // watermark.  The allocator then shrinks the limit exactly at admission:
    // the first attempt is rejected, while spilling this store's current
    // block creates enough headroom for the single retry.
    quota.set_limit(aggregate_bytes + 4 * ObRADatumStore::BIG_BLOCK_SIZE);
    ASSERT_GT(local_limit, quota.limit());
    ASSERT_FALSE(should_spill_workarea(
        local_limit, first_store.get_mem_hold(),
        ObRADatumStore::BIG_BLOCK_SIZE));
    race_allocator.arm(first_store.get_mem_hold() / 2);
    const int64_t reject_count = quota.reject_count();
    const int64_t reclaim_count = quota.reclaim_count();

    row.at(0).set_string(second_payload.data(), second_payload.size());
    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    EXPECT_TRUE(race_allocator.triggered());
    EXPECT_EQ(2, first_store.get_row_cnt());
    EXPECT_EQ(1, second_store.get_row_cnt());
    EXPECT_GT(quota.reject_count(), reject_count);
    EXPECT_GT(quota.reclaim_count(), reclaim_count);
    EXPECT_GT(first_store.spilled_bytes(), 0);
    EXPECT_LE(quota.committed(), quota.limit());
    ASSERT_EQ(OB_SUCCESS, first_store.finish_add_row());

    quota.set_limit(quota.committed() + 2 * ObRADatumStore::BIG_BLOCK_SIZE);
    ObRADatumStore::Reader reader(first_store);
    const ObRADatumStore::StoredRow *stored_row = nullptr;
    ASSERT_EQ(OB_SUCCESS, reader.get_row(0, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(first_payload.size(), first_payload.data()),
              stored_row->cells()[0].get_string());
    ASSERT_EQ(OB_SUCCESS, reader.get_row(1, stored_row));
    ASSERT_NE(nullptr, stored_row);
    EXPECT_EQ(ObString(second_payload.size(), second_payload.data()),
              stored_row->cells()[0].get_string());
    EXPECT_EQ(OB_INDEX_OUT_OF_RANGE, reader.get_row(2, stored_row));
  }

  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

TEST(TestWorkareaMemoryQuota,
     chunk_row_store_reuses_spilled_block_after_quota_admission_race)
{
  TestTmpFileManagerGuard tmp_file_manager;
  ASSERT_EQ(OB_SUCCESS, tmp_file_manager.init());
  MemoryQuota quota;
  TestWorkareaQuotaResolverGuard quota_resolver_guard(quota);
  ObSqlMemoryTracker tracker;
  tracker.set_memory_quota(&quota);
  DefaultPageAllocator backing_allocator(
      ObMemAttr("WAChunkRowAlloc", ObCtxIds::WORK_AREA));
  TrackedAllocator quota_allocator(
      backing_allocator, nullptr, &quota,
      ObMemAttr("WAChunkRowAlloc", ObCtxIds::WORK_AREA));
  ShrinkQuotaOnNextAllocation race_allocator(quota_allocator, quota);
  std::vector<char> first_payload(40L << 10, 'a');
  std::vector<char> second_payload(40L << 10, 'b');
  ObObj cell;
  cell.set_varchar(ObString(first_payload.size(), first_payload.data()));
  ObNewRow row;
  row.cells_ = &cell;
  row.count_ = 1;

  {
    TestChunkRowStore first_store(race_allocator, tracker);
    TestChunkRowStore second_store(race_allocator, tracker);
    ASSERT_EQ(OB_SUCCESS,
              first_store.init(0, ObCtxIds::WORK_AREA,
                               "WAChunkRowFirst", true));
    ASSERT_EQ(OB_SUCCESS,
              second_store.init(0, ObCtxIds::WORK_AREA,
                                "WAChunkRowSecond", true));
    ASSERT_EQ(OB_SUCCESS, first_store.alloc_dir_id());
    first_store.set_callback(&tracker);
    second_store.set_callback(&tracker);

    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    ASSERT_EQ(OB_SUCCESS, second_store.add_row(row));
    const int64_t aggregate_bytes = quota.committed();
    ASSERT_GT(aggregate_bytes, 0);
    quota.set_limit(aggregate_bytes + 4 * ObChunkRowStore::BLOCK_SIZE);
    ASSERT_FALSE(should_spill_workarea(
        0, first_store.get_mem_hold(), ObChunkRowStore::BLOCK_SIZE));
    race_allocator.arm(first_store.get_mem_hold() / 2);
    const int64_t reject_count = quota.reject_count();
    const int64_t reclaim_count = quota.reclaim_count();

    cell.set_varchar(ObString(second_payload.size(), second_payload.data()));
    ASSERT_EQ(OB_SUCCESS, first_store.add_row(row));
    EXPECT_TRUE(race_allocator.triggered());
    EXPECT_EQ(2, first_store.get_row_cnt());
    EXPECT_EQ(1, second_store.get_row_cnt());
    EXPECT_GT(quota.reject_count(), reject_count);
    EXPECT_GT(quota.reclaim_count(), reclaim_count);
    EXPECT_GT(first_store.spilled_bytes(), 0);
    EXPECT_LE(quota.committed(), quota.limit());
    ASSERT_EQ(OB_SUCCESS, first_store.finish_add_row(false));

    quota.set_limit(quota.committed() + 2 * ObChunkRowStore::BLOCK_SIZE);
    ObChunkRowStore::Iterator iterator;
    ASSERT_EQ(OB_SUCCESS, first_store.begin(iterator));
    ObNewRow read_row;
    ObObj read_cell;
    read_row.cells_ = &read_cell;
    read_row.count_ = 1;
    ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(read_row));
    EXPECT_EQ(ObString(first_payload.size(), first_payload.data()),
              read_row.cells_[0].get_string());
    ASSERT_EQ(OB_SUCCESS, iterator.get_next_row(read_row));
    EXPECT_EQ(ObString(second_payload.size(), second_payload.data()),
              read_row.cells_[0].get_string());
    EXPECT_EQ(OB_ITER_END, iterator.get_next_row(read_row));
  }

  EXPECT_EQ(0, quota.committed());
  EXPECT_EQ(0, quota.reserved());
}

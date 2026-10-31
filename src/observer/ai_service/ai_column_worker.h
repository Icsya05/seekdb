#pragma once

#include "share/ob_thread_pool.h"

namespace oceanbase
{
namespace observer
{
class AiColumnWorker final : public share::ObThreadPool
{
public:
  AiColumnWorker() : share::ObThreadPool(4) {}
  ~AiColumnWorker() override { stop(); wait(); }
private:
  void run1() override;
  int process_table(uint64_t table_id, bool &did_work);
};
} // namespace observer
} // namespace oceanbase

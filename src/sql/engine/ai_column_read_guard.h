#pragma once
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace sql
{
class ObDMLStmt;
class ObExecContext;
class AiColumnReadGuard
{
public:
  static int collect(ObDMLStmt &stmt, common::ObIArray<uint64_t> &references);
  static int check(ObExecContext &context, const common::ObIArray<uint64_t> &references);
};
} // namespace sql
} // namespace oceanbase

/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase {
namespace sql {
class ObSQLSessionInfo;
namespace file_sql {
int to_sql_error(int status, const std::string &error);
int get_secure_dir(ObSQLSessionInfo &session, common::ObIAllocator &allocator, common::ObString &dir);
}

class FileScanSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  FileScanSpec(common::ObIAllocator &allocator, const ObPhyOperatorType type)
      : ObOpSpec(allocator, type), binding_expr_(nullptr), column_exprs_(allocator), source_column_ids_(allocator) {}
  ObExpr *binding_expr_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> source_column_ids_;
};

class FileScanOp : public ObOperator
{
public:
  FileScanOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(ctx, spec, input), scanner_(), binding_(), row_(), ended_(false) {}
  int inner_open() override;
  int inner_rescan() override;
  int inner_get_next_row() override;
  int inner_get_next_batch(int64_t max_row_cnt) override;
  int inner_close() override;
  void destroy() override;
private:
  int open_scanner();
  int fetch_row(int64_t row_idx);
  int set_cell(const file_sql::Cell &cell, const file_sql::Column &column,
               const ObExpr &expr, common::ObDatum &datum, int64_t row_idx);
  file_sql::Scanner scanner_;
  file_sql::Binding binding_;
  std::vector<file_sql::Cell> row_;
  bool ended_;
};
} // namespace sql
} // namespace oceanbase

/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_op.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/charset/ob_charset.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace oceanbase {
using namespace common;
namespace sql {
namespace file_sql {
int to_sql_error(int status, const std::string &error)
{
  int ret = OB_INVALID_ARGUMENT;
  if (status <= OB_ERROR) {
    // Inference and long records return the original timeout/kill status.
    ret = status;
  } else if (status == DENIED) {
    ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    LOG_USER_ERROR(OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS);
  } else if (status == UNSUPPORTED) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, error.c_str());
  } else {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, error.c_str());
  }
  LOG_WARN("file SQL scan failed", K(ret), K(status), "diagnostic", error.c_str());
  return ret;
}

int get_secure_dir(ObSQLSessionInfo &session, ObIAllocator &allocator, ObString &dir)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_FAIL(session.get_enable_file_sql(enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled");
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
      *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, allocator, dir))) {
  }
  return ret;
}
} // namespace file_sql

OB_SERIALIZE_MEMBER((FileScanSpec, ObOpSpec), binding_expr_, column_exprs_, source_column_ids_);

int FileScanOp::open_scanner()
{
  int ret = OB_SUCCESS;
  scanner_.close();
  ended_ = false;
  ObDatum *datum = nullptr;
  ObString secure_dir;
  std::string error;
  if (OB_ISNULL(ctx_.get_my_session()) || OB_ISNULL(MY_SPEC.binding_expr_)
      || MY_SPEC.column_exprs_.count() != MY_SPEC.source_column_ids_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(file_sql::get_secure_dir(*ctx_.get_my_session(), ctx_.get_allocator(), secure_dir))) {
  } else if (OB_FAIL(MY_SPEC.binding_expr_->eval(eval_ctx_, datum))) {
  } else if (datum->is_null()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObString json = datum->get_string();
    int status = file_sql::deserialize_binding(std::string(json.ptr(), json.length()), binding_, error);
    if (status == file_sql::OK) {
      status = scanner_.open(binding_, std::string(secure_dir.ptr(), secure_dir.length()), error);
    }
    if (status != file_sql::OK) { ret = file_sql::to_sql_error(status, error); }
  }
  return ret;
}

int FileScanOp::inner_open() { return open_scanner(); }

int FileScanOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    // Reopen checks the current authorization as well as the bound fingerprint.
    ret = open_scanner();
  }
  return ret;
}

int FileScanOp::set_cell(const file_sql::Cell &cell, const file_sql::Column &column,
                         const ObExpr &expr, ObDatum &datum, int64_t row_idx)
{
  int ret = OB_SUCCESS;
  if (cell.is_null || column.type == file_sql::Type::NULL_VALUE) {
    datum.set_null();
  } else {
    const ObString text(static_cast<int32_t>(cell.text.size()), cell.text.data());
    switch (column.type) {
      case file_sql::Type::BIGINT: {
        char *end = nullptr;
        errno = 0;
        const int64_t value = std::strtoll(cell.text.c_str(), &end, 10);
        if (errno == ERANGE || end != cell.text.c_str() + cell.text.size()) { ret = OB_INVALID_ARGUMENT; }
        else { datum.set_int(value); }
        break;
      }
      case file_sql::Type::DOUBLE: {
        char *end = nullptr;
        int error = 0;
        const double value = ObCharset::strntod(cell.text.c_str(), cell.text.size(), &end, &error);
        if (error != 0 || !std::isfinite(value) || end != cell.text.c_str() + cell.text.size()) { ret = OB_INVALID_ARGUMENT; }
        else { datum.set_double(value); }
        break;
      }
      case file_sql::Type::BOOLEAN:
        datum.set_int(text.case_compare("true") == 0 ? 1 : 0);
        break;
      case file_sql::Type::DATE: {
        int32_t value = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(text, value))) {
        } else { datum.set_date(value); }
        break;
      }
      case file_sql::Type::DATETIME: {
        int64_t value = 0;
        const ObTimeConvertCtx convert(ctx_.get_my_session()->get_timezone_info(), false);
        if (OB_FAIL(ObTimeConverter::str_to_datetime(text, convert, value))) {
        } else { datum.set_datetime(value); }
        break;
      }
      case file_sql::Type::VARCHAR: {
        ObTextStringDatumResult result(expr.datum_meta_.type_, &expr, &eval_ctx_, &datum);
        if (OB_FAIL(result.init_with_batch_idx(cell.text.size(), row_idx))) {
        } else if (OB_FAIL(result.append(text))) {
        } else { result.set_result(); }
        break;
      }
      case file_sql::Type::NULL_VALUE: datum.set_null(); break;
    }
  }
  return ret;
}

int FileScanOp::fetch_row(int64_t row_idx)
{
  int ret = OB_SUCCESS;
  std::string error;
  if (ended_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    const int status = scanner_.next(row_, error, [this]() { return ctx_.check_status(); });
    if (status == file_sql::END) {
      ended_ = true;
      ret = OB_ITER_END;
    } else if (status != file_sql::OK) {
      ret = file_sql::to_sql_error(status, error);
    } else if (row_.size() != binding_.columns.size()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
        const int64_t source = MY_SPEC.source_column_ids_.at(i);
        const ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        if (OB_ISNULL(expr) || source < 0 || source >= static_cast<int64_t>(row_.size())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ObDatum &datum = expr->locate_expr_datum(eval_ctx_, row_idx);
          if (OB_FAIL(set_cell(row_[source], binding_.columns[source], *expr, datum, row_idx))) {
          } else {
            if (expr->is_batch_result()) {
              expr->get_evaluated_flags(eval_ctx_).set(row_idx);
            }
            expr->set_evaluated_projected(eval_ctx_);
          }
        }
      }
    }
  }
  return ret;
}

int FileScanOp::inner_get_next_row()
{
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_size(1);
  guard.set_batch_idx(0);
  clear_evaluated_flag();
  return fetch_row(0);
}

int FileScanOp::inner_get_next_batch(int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  clear_datum_eval_flag();
  const int64_t capacity = MIN(max_row_cnt, MY_SPEC.max_batch_size_);
  if (capacity <= 0) { return OB_INVALID_ARGUMENT; }
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_size(capacity);
  brs_.size_ = 0;
  brs_.end_ = false;
  brs_.skip_->reset(capacity);
  for (int64_t i = 0; OB_SUCC(ret) && i < capacity; ++i) {
    guard.set_batch_idx(i);
    ret = fetch_row(i);
    if (ret == OB_ITER_END) {
      brs_.end_ = true;
      ret = OB_SUCCESS;
      break;
    } else if (OB_SUCC(ret)) {
      ++brs_.size_;
    }
  }
  brs_.all_rows_active_ = true;
  return ret;
}

int FileScanOp::inner_close()
{
  scanner_.close();
  row_.clear();
  ended_ = true;
  return OB_SUCCESS;
}

void FileScanOp::destroy()
{
  scanner_.close();
  scanner_.~Scanner();
  binding_.~Binding();
  row_.~vector();
  ObOperator::destroy();
}
} // namespace sql
} // namespace oceanbase

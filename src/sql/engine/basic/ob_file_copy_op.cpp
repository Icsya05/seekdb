/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_select_into_op.h"
#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/ob_file_scan.h"
#include "sql/engine/ob_exec_context.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"

namespace oceanbase { namespace sql {
using namespace common;
namespace {
int validate_inputs(ObOperator *op, int depth = 0)
{
  if (op == nullptr || depth > 512) { return OB_SIZE_OVERFLOW; }
  int ret = OB_SUCCESS;
  if (op->get_spec().type_ == PHY_FUNCTION_TABLE) { ret = static_cast<ObFunctionTableOp *>(op)->validate_file_input(); }
  for (uint32_t i = 0; OB_SUCC(ret) && i < op->get_child_cnt(); ++i) { ret = validate_inputs(op->get_child(i),depth+1); }
  return ret;
}
}

int ObSelectIntoOp::open_file_copy()
{
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObString root;
  FileScanDefinition options;
  if (ctx_.get_my_session() == nullptr || child_ == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ctx_.get_my_session()->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled))) { }
  else if (enabled.get_int() == 0) { ret = OB_NOT_SUPPORTED; }
  else if (OB_FAIL(ctx_.get_my_session()->get_sql_select_limit(top_limit_cnt_))) { }
  else if (OB_FAIL(options.decode(MY_SPEC.external_properties_.str_))) { }
  else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                    *GCTX.schema_service_,share::SYS_VAR_SECURE_FILE_PRIV,ctx_.get_allocator(),root))) { }
  else if ((file_copy_writer_ = OB_NEWx(FileCsvWriter,&ctx_.get_allocator())) == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else { ret = file_copy_writer_->open(options,root); }
  return ret;
}
int ObSelectIntoOp::write_file_copy_row()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj,16> values;
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.select_exprs_.count(); ++i) {
    const ObExpr *expr = MY_SPEC.select_exprs_.at(i);
    ObDatum *datum = nullptr;
    ObObj obj;
    if (expr == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(expr->eval(eval_ctx_,datum))) { }
    else if (OB_FAIL(datum->to_obj(obj,expr->obj_meta_))) { }
    else if (ob_is_text_tc(obj.get_type())) { ret = OB_NOT_SUPPORTED; }
    else { ret = values.push_back(obj); }
  }
  return OB_SUCC(ret) ? file_copy_writer_->write_row(values) : ret;
}
int ObSelectIntoOp::execute_file_copy(bool batch, int64_t max_rows)
{
  if (batch) { brs_.size_ = 0; brs_.end_ = true; }
  if (file_copy_done_) { return batch ? OB_SUCCESS : OB_ITER_END; }
  if (file_copy_writer_ == nullptr || child_ == nullptr || ctx_.get_physical_plan_ctx() == nullptr) { return OB_NOT_INIT; }
  int ret = OB_SUCCESS;
  int64_t rows = 0;
  while (OB_SUCC(ret) && rows < top_limit_cnt_) {
    clear_evaluated_flag();
    if (OB_FAIL(ctx_.check_status())) { break; }
    if (!batch) {
      ret = child_->get_next_row();
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; break; }
      if (OB_SUCC(ret)) { ret = write_file_copy_row(); ++rows; }
    } else {
      const ObBatchRows *result = nullptr;
      int64_t count = std::min<int64_t>(std::min(max_rows,MY_SPEC.max_batch_size_),top_limit_cnt_-rows);
      if (count <= 0) { ret = OB_INVALID_ARGUMENT; break; }
      if (OB_FAIL(child_->get_next_batch(count,result))) { break; }
      ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
      guard.set_batch_size(result->size_);
      for (int64_t i = 0; OB_SUCC(ret) && i < result->size_; ++i) {
        if (result->skip_->contain(i)) { continue; }
        guard.set_batch_idx(i);
        ret = write_file_copy_row(); ++rows;
      }
      if (result->end_) { break; }
    }
  }
  if (OB_SUCC(ret)) { ret = ctx_.check_status(); }
  if (OB_SUCC(ret)) { ret = validate_inputs(child_); }
  if (OB_SUCC(ret)) { ret = file_copy_writer_->finish(); }
  if (OB_SUCC(ret)) {
    ctx_.get_physical_plan_ctx()->set_affected_rows(rows);
    file_copy_done_ = true;
    if (!batch) { ret = OB_ITER_END; }
  }
  return ret;
}
} }

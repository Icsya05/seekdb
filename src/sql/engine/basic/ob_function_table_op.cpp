/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/basic/ob_file_scan.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    is_file_table_, file_column_indices_);

int ObFunctionTableOp::open_file_reader()
{
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObDatum *metadata = nullptr;
  ObString secure_root;
  FileScanDefinition definition;
  if (file_reader_ != nullptr) { file_reader_->close(); }
  if (OB_ISNULL(ctx_.get_my_session())) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ctx_.get_my_session()->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) { }
  else if (enabled.get_int() == 0) { ret = OB_NOT_SUPPORTED; }
  else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, metadata))) { }
  else if (metadata->is_null()) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(definition.decode(metadata->get_string()))) { }
  else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                       *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV,
                       ctx_.get_allocator(), secure_root))) { }
  else {
    if (file_reader_ == nullptr) {
      file_reader_ = OB_NEWx(FileScanReader, &ctx_.get_allocator());
      if (file_reader_ == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    }
    if (OB_SUCC(ret)) { ret = file_reader_->open(definition, secure_root, &MY_SPEC.file_column_indices_); }
  }
  return ret;
}

int ObFunctionTableOp::validate_file_input() const
{
  return file_reader_ == nullptr ? OB_SUCCESS : file_reader_->check_unchanged();
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  clear_evaluated_flag();
  file_row_allocator_.reuse();
  return fill_file_row();
}

int ObFunctionTableOp::inner_get_next_batch(int64_t max_row_cnt)
{
  if (!MY_SPEC.is_file_table_) {
    // Existing collection functions keep their scalar semantics even when a
    // parent vector operator requests a batch. Their buffers own this row.
    ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
    guard.set_batch_size(1); guard.set_batch_idx(0);
    brs_.size_ = 0; brs_.end_ = false; brs_.skip_->reset(1);
    int ret = inner_get_next_row();
    if (ret == OB_ITER_END) { brs_.end_ = true; ret = OB_SUCCESS; }
    else if (OB_SUCC(ret)) { brs_.size_ = 1; }
    return ret;
  }
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  file_row_allocator_.reuse();
  int64_t size = std::min<int64_t>(16, std::min(max_row_cnt, MY_SPEC.max_batch_size_));
  if (size <= 0) { return OB_INVALID_ARGUMENT; }
  brs_.size_ = 0; brs_.end_ = false; brs_.skip_->reset(size);
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_size(size);
  while (OB_SUCC(ret) && brs_.size_ < size) {
    guard.set_batch_idx(brs_.size_);
    ret = fill_file_row();
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; brs_.end_ = true; break; }
    if (OB_SUCC(ret)) {
      ++brs_.size_;
      if (file_row_allocator_.used() >= FileScanReader::MAX_RECORD_BYTES) { break; }
    }
  }
  return ret;
}

int ObFunctionTableOp::fill_file_row()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 16> values;
  if (OB_ISNULL(file_reader_)) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ctx_.check_status())) { }
  else if (OB_FAIL(file_reader_->next(file_row_allocator_, values))) { }
  else if (MY_SPEC.column_exprs_.count() != MY_SPEC.file_column_indices_.count()) { ret = OB_ERR_UNEXPECTED; }
  else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      int64_t index = MY_SPEC.file_column_indices_.at(i);
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (index < 0 || index >= values.count() || expr == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(expr->locate_datum_for_write(eval_ctx_).from_obj(values.at(index), expr->obj_datum_map_))) { }
      else { expr->set_evaluated_projected(eval_ctx_); }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.is_file_table_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    ret = open_file_reader();
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.is_file_table_) {
    ret = open_file_reader();
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (file_reader_ != nullptr) {
    ret = file_reader_->check_unchanged();
    file_reader_->close();
  }
  file_row_allocator_.reset();
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (file_reader_ != nullptr) {
    file_reader_->~FileScanReader();
    ctx_.get_allocator().free(file_reader_);
    file_reader_ = nullptr;
  }
  file_row_allocator_.reset();
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase

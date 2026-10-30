/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/engine/basic/ob_file_scan_op.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/ob_sql_context.h"
#include <set>

namespace oceanbase {
using namespace common;
namespace sql {

int ObDMLResolver::resolve_file_table_item(const ParseNode &node, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  table_item = nullptr;
  bool enabled = false;
  ObString secure_dir;
  file_sql::Format format = file_sql::Format::CSV;
  file_sql::Options options;
  file_sql::Binding binding;
  std::string error;
  std::set<std::string> seen;
  if (OB_ISNULL(session_info_) || OB_ISNULL(allocator_) || OB_ISNULL(get_stmt())
      || OB_ISNULL(session_info_->get_cur_exec_ctx()) || node.num_child_ != 4
      || OB_ISNULL(node.children_[0]) || OB_ISNULL(node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session_info_->get_enable_file_sql(enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; SET SESSION enable_file_sql=ON");
  } else if (OB_FAIL(file_sql::get_secure_dir(*session_info_, *allocator_, secure_dir))) {
  } else {
    const ParseNode &name = *node.children_[0];
    const ObString function_name(static_cast<int32_t>(name.str_len_), name.str_value_);
    if (function_name.case_compare("read_csv") == 0) {
      format = file_sql::Format::CSV;
    } else if (function_name.case_compare("read_json") == 0 || function_name.case_compare("read_ndjson") == 0) {
      format = file_sql::Format::JSONL;
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown file table function");
    }
    const ParseNode *params = node.children_[2];
    for (int64_t i = 0; OB_SUCC(ret) && params != nullptr && i < params->num_child_; ++i) {
      const ParseNode *param = params->children_[i];
      if (OB_ISNULL(param) || param->num_child_ != 2 || OB_ISNULL(param->children_[0]) || OB_ISNULL(param->children_[1])) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const ParseNode &key = *param->children_[0];
        const ParseNode &value = *param->children_[1];
        std::string option(key.str_value_, key.str_len_);
        for (char &c : option) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
        if (!seen.insert(option).second) {
          ret = OB_INVALID_ARGUMENT;
        } else if (option == "header" && format == file_sql::Format::CSV && value.type_ == T_BOOL) {
          options.header = value.value_ != 0;
        } else if (option == "sample_size" && value.type_ == T_INT) {
          options.sample_size = value.value_;
        } else if (option == "format" && format == file_sql::Format::JSONL && value.type_ == T_VARCHAR) {
          const ObString format_name(static_cast<int32_t>(value.str_len_), value.str_value_);
          if (format_name.case_compare("newline_delimited") != 0 && format_name.case_compare("ndjson") != 0) {
            ret = OB_NOT_SUPPORTED;
          }
        } else if (format == file_sql::Format::CSV && value.type_ == T_VARCHAR) {
          if (option == "nullstr") {
            options.nullstr.assign(value.str_value_, value.str_len_);
          } else if (value.str_len_ != 1) {
            ret = OB_INVALID_ARGUMENT;
          } else if (option == "delimiter" || option == "delim") {
            options.delimiter = value.str_value_[0];
          } else if (option == "quote") {
            options.quote = value.str_value_[0];
          } else if (option == "escape") {
            options.escape = value.str_value_[0];
          } else {
            ret = OB_INVALID_ARGUMENT;
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
        if (OB_FAIL(ret)) {
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid or duplicate file table option");
        }
      }
    }
    if (OB_SUCC(ret) && options.sample_size != -1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL requires full schema inference: sample_size=-1");
    }
    if (OB_SUCC(ret)) {
      const ParseNode &path = *node.children_[1];
      const int status = file_sql::infer_file(std::string(path.str_value_, path.str_len_),
          std::string(secure_dir.ptr(), secure_dir.length()), format, options, binding, error,
          [this]() { return session_info_->get_cur_exec_ctx()->check_status(); });
      if (status != file_sql::OK) { ret = file_sql::to_sql_error(status, error); }
    }
    std::string serialized;
    ObString owned_binding;
    ObConstRawExpr *binding_expr = nullptr;
    if (OB_FAIL(ret)) {
    } else if (file_sql::serialize_binding(binding, serialized, error) != file_sql::OK) {
      ret = file_sql::to_sql_error(file_sql::INVALID, error);
    } else if (OB_FAIL(ob_write_string(*allocator_, ObString(static_cast<int32_t>(serialized.size()), serialized.data()), owned_binding))) {
    } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType,
        owned_binding, CS_TYPE_UTF8MB4_BIN, binding_expr))) {
    } else if (OB_ISNULL(table_item = get_stmt()->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObString alias;
      if (node.children_[3] != nullptr) {
        alias.assign_ptr(node.children_[3]->str_value_, static_cast<int32_t>(node.children_[3]->str_len_));
      } else {
        ret = get_stmt()->generate_func_table_name(*allocator_, alias);
      }
      if (OB_SUCC(ret)) {
        table_item->type_ = TableItem::FUNCTION_TABLE;
        table_item->is_file_table_ = true;
        table_item->is_system_table_ = false;
        table_item->table_id_ = generate_table_id();
        table_item->table_name_ = alias;
        table_item->alias_name_ = alias;
        table_item->function_table_expr_ = binding_expr;
        ret = get_stmt()->add_table_item(session_info_, table_item);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(binding.columns.size()); ++i) {
        const file_sql::Column &column = binding.columns[i];
        ObObjMeta meta;
        ObAccuracy accuracy;
        switch (column.type) {
          case file_sql::Type::BIGINT: meta.set_int(); accuracy.set_precision(19); accuracy.set_scale(0); break;
          case file_sql::Type::DOUBLE: meta.set_double(); break;
          case file_sql::Type::BOOLEAN: meta.set_tinyint(); accuracy.set_precision(1); accuracy.set_scale(0); break;
          case file_sql::Type::DATE: meta.set_date(); break;
          case file_sql::Type::DATETIME:
            meta.set_datetime(); accuracy.set_scale(MAX(0, column.max_length - 20)); break;
          case file_sql::Type::NULL_VALUE: meta.set_null(); break;
          case file_sql::Type::VARCHAR:
            meta.set_varchar(); meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            accuracy.set_length(MAX(1, column.max_length)); break;
        }
        ObString column_name;
        ColumnItem *item = nullptr;
        if (OB_FAIL(ob_write_string(*allocator_, ObString(static_cast<int32_t>(column.name.size()), column.name.data()), column_name))) {
        }
        for (int64_t j = 0; OB_SUCC(ret) && j < get_stmt()->get_column_size(); ++j) {
          const ColumnItem *existing = get_stmt()->get_column_item(j);
          if (existing->table_id_ == table_item->table_id_
              && ObCharset::case_insensitive_equal(existing->column_name_, column_name)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ambiguous file column names under SQL collation");
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(resolve_function_table_column_item(*table_item, meta, accuracy,
            column_name, OB_APP_MIN_COLUMN_ID + i, item))) {
        } else if (column.nullable || column.type == file_sql::Type::NULL_VALUE) {
          item->expr_->unset_result_flag(NOT_NULL_FLAG);
        } else {
          item->expr_->set_result_flag(NOT_NULL_FLAG);
        }
      }
      if (OB_SUCC(ret)) {
        ObQueryCtx *query_ctx = get_stmt()->get_query_ctx();
        query_ctx->disable_udf_parallel_ = true;
        ObGlobalHint &hint = query_ctx->get_query_hint_for_update().get_global_hint();
        hint.merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
        hint.parallel_ = 1;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

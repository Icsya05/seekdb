/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/engine/basic/ob_file_scan.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;

int ObDMLResolver::resolve_file_table_item(const ParseNode &tree, TableItem *&item)
{
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObString secure_root;
  FileScanDefinition definition;
  if (tree.num_child_ != 3 || tree.children_[0] == nullptr || tree.children_[1] == nullptr) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (OB_FAIL(session_info_->get_sys_variable(SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (enabled.get_int() == 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; set enable_file_sql=ON");
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                       *GCTX.schema_service_, SYS_VAR_SECURE_FILE_PRIV, *allocator_, secure_root))) {
  }
  if (OB_SUCC(ret)) {
    const ParseNode &name_node = *tree.children_[0];
    ObString name(name_node.str_len_, name_node.str_value_);
    if (name.case_compare("read_csv") == 0) { definition.format = FileScanDefinition::CSV; }
    else if (name.case_compare("read_json") == 0 || name.case_compare("read_ndjson") == 0) { definition.format = FileScanDefinition::JSONL; }
    else if (name.case_compare("read_parquet") == 0) { definition.format = FileScanDefinition::PARQUET; }
    else if (name.case_compare("glob") == 0) { definition.format = FileScanDefinition::GLOB; }
    else { ret = OB_ERR_PARSER_SYNTAX; }
  }
  auto constant = [&](const ParseNode &node, ObObj &value) {
    int ret = OB_SUCCESS;
    ObRawExpr *expr = nullptr;
    bool got = false;
    if (OB_FAIL(resolve_sql_expr(node, expr))) { }
    else if (expr == nullptr || expr->has_flag(CNT_COLUMN) || expr->has_flag(CNT_SUB_QUERY)) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(session_info_->get_cur_exec_ctx(),
                                                             expr, value, got, *allocator_))) { }
    else if (!got || value.is_null()) { ret = OB_INVALID_ARGUMENT; }
    return ret;
  };
  const ParseNode *args = tree.children_[1];
  std::set<std::string> options;
  if (OB_SUCC(ret) && args->num_child_ < 1) { ret = OB_ERR_PARSER_SYNTAX; }
  for (int64_t i = 0; OB_SUCC(ret) && i < args->num_child_; ++i) {
    const ParseNode *arg = args->children_[i];
    ObObj value;
    if (arg == nullptr) { ret = OB_ERR_PARSER_SYNTAX; }
    else if (i == 0) {
      if (OB_FAIL(constant(*arg, value))) { }
      else if (!value.is_string_type()) { ret = OB_INVALID_ARGUMENT; }
      else { definition.path.assign(value.get_string_ptr(), value.get_string_len()); }
    } else if (arg->type_ != T_OP_EQ || arg->num_child_ != 2 || arg->children_[0] == nullptr
               || arg->children_[1] == nullptr || arg->children_[0]->type_ != T_COLUMN_REF
               || arg->children_[0]->num_child_ != 3 || arg->children_[0]->children_[2] == nullptr
               || arg->children_[0]->children_[0] != nullptr || arg->children_[0]->children_[1] != nullptr) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      const ParseNode *key = arg->children_[0]->children_[2];
      std::string option(key->str_value_, key->str_len_);
      for (char &c : option) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
      if (!options.insert(option).second) { ret = OB_INVALID_ARGUMENT; }
      else if (OB_FAIL(constant(*arg->children_[1], value))) { }
      else if (option == "sample_size") {
        if (!value.is_integer_type() || value.get_int() != -1) { ret = OB_NOT_SUPPORTED; }
      } else if (option == "header" && definition.format == FileScanDefinition::CSV) {
        if (!value.is_integer_type() || (value.get_int() != 0 && value.get_int() != 1)) { ret = OB_INVALID_ARGUMENT; }
        else { definition.header = value.get_int() != 0; }
      } else if (option == "format" && definition.format == FileScanDefinition::JSONL) {
        if (!value.is_string_type() || value.get_string().case_compare("newline_delimited") != 0) { ret = OB_NOT_SUPPORTED; }
      } else if (definition.format == FileScanDefinition::CSV && value.is_string_type()) {
        std::string text(value.get_string_ptr(), value.get_string_len());
        if (option == "nullstr" || option == "null_string") { definition.null_text = text; }
        else if (option == "delimiter" || option == "delim" || option == "sep") {
          if (text.size() != 1 || text[0] == '\r' || text[0] == '\n' || text[0] == 0) { ret = OB_INVALID_ARGUMENT; }
          else { definition.delimiter = text[0]; }
        } else if (option == "quote" || option == "escape") {
          if (text.size() > 1 || (!text.empty() && (text[0] == 0 || text[0] == '\r' || text[0] == '\n'))) { ret = OB_INVALID_ARGUMENT; }
          else if (option == "quote") { definition.quote = text.empty() ? 0 : text[0]; }
          else { definition.escape = text.empty() ? 0 : text[0]; }
        } else { ret = OB_INVALID_ARGUMENT; }
      } else { ret = OB_INVALID_ARGUMENT; }
    }
  }
  if (OB_SUCC(ret) && definition.quote != 0 && definition.quote == definition.delimiter) { ret = OB_INVALID_ARGUMENT; }
  FileScanReader reader;
  ObString encoded;
  ObConstRawExpr *metadata = nullptr;
  if (OB_FAIL(ret)) { }
  else if (OB_FAIL(reader.infer(definition, secure_root))) { }
  else if (OB_FAIL(definition.encode(*allocator_, encoded))) { }
  else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType,
                                                        encoded, CS_TYPE_UTF8MB4_BIN, metadata))) { }
  else if ((item = get_stmt()->create_table_item(*allocator_)) == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else {
    ObString alias;
    if (tree.children_[2] != nullptr) {
      alias.assign_ptr(tree.children_[2]->str_value_, tree.children_[2]->str_len_);
    } else {
      alias.assign_ptr(tree.children_[0]->str_value_, tree.children_[0]->str_len_);
    }
    item->type_ = TableItem::FUNCTION_TABLE;
    item->is_file_table_ = true;
    item->is_system_table_ = false;
    item->table_id_ = generate_table_id();
    item->table_name_ = alias;
    item->alias_name_ = alias;
    item->function_table_expr_ = metadata;
    if (OB_FAIL(get_stmt()->add_table_item(session_info_, item))) { }
    else {
      ObSEArray<ColumnItem, 16> columns;
      ret = resolve_file_table_columns(*item, columns);
    }
    // External file schemas are not catalog objects and can change between SQL
    // executions. Infer again instead of caching a plan with stale columns.
    ObQueryHint &hint = get_stmt()->get_query_ctx()->get_query_hint_for_update();
    const_cast<ObGlobalHint &>(hint.get_global_hint()).plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_columns(const TableItem &item, ObIArray<ColumnItem> &columns)
{
  int ret = OB_SUCCESS;
  FileScanDefinition definition;
  if (item.function_table_expr_ == nullptr || !item.function_table_expr_->is_const_expr()) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(definition.decode(static_cast<ObConstRawExpr *>(item.function_table_expr_)->get_value().get_string()))) { }
  for (size_t i = 0; OB_SUCC(ret) && i < definition.columns.size(); ++i) {
    const FileColumn &column = definition.columns[i];
    ObString name;
    ColumnItem *existing = get_stmt()->get_column_item_by_id(item.table_id_, OB_APP_MIN_COLUMN_ID + i);
    if (existing == nullptr) {
      ObObjMeta meta;
      meta.set_type(FileScanDefinition::object_type(column.type));
      meta.set_collation_type(meta.is_string_type() ? CS_TYPE_UTF8MB4_BIN : CS_TYPE_BINARY);
      meta.set_collation_level(meta.is_string_type() ? CS_LEVEL_IMPLICIT : CS_LEVEL_NUMERIC);
      ObAccuracy accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[meta.get_type()];
      if (meta.is_string_type()) { accuracy.set_length(FileScanReader::MAX_RECORD_BYTES); }
      if (column.type == FileValueType::DATETIME) { accuracy.set_scale(column.scale); }
      if (OB_FAIL(ob_write_string(*allocator_, ObString(column.name.size(), column.name.data()), name))) { }
      else if (OB_FAIL(resolve_function_table_column_item(item, meta, accuracy, name, OB_APP_MIN_COLUMN_ID+i, existing))) { }
      else if (!column.nullable) { existing->expr_->set_result_flag(NOT_NULL_FLAG); }
    }
    if (OB_SUCC(ret)) { ret = columns.push_back(*existing); }
  }
  return ret;
}
} }

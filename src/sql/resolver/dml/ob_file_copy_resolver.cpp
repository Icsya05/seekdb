/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/engine/basic/ob_file_scan.h"
#include "sql/ob_sql_utils.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
int ObSelectResolver::resolve_file_copy_options(const ParseNode &node, ObSelectIntoItem &item)
{
  int ret = OB_SUCCESS;
  ObObj enabled;
  FileScanDefinition options;
  options.header = false;
  auto constant = [&](const ParseNode &node, ObObj &value) {
    int ret = OB_SUCCESS;
    ObRawExpr *expr = nullptr;
    bool valid = false;
    if (OB_FAIL(resolve_sql_expr(node,expr))) { }
    else if (expr == nullptr || expr->has_flag(CNT_COLUMN) || expr->has_flag(CNT_SUB_QUERY)) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(session_info_->get_cur_exec_ctx(),expr,value,valid,*allocator_))) { }
    else if (!valid || value.is_null()) { ret = OB_INVALID_ARGUMENT; }
    return ret;
  };
  if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) { }
  else if (enabled.get_int() == 0) { ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled"); }
  else if (OB_FAIL(constant(*node.children_[0],item.outfile_name_))) { }
  else if (!item.outfile_name_.is_string_type()) { ret = OB_INVALID_ARGUMENT; }
  else { options.path.assign(item.outfile_name_.get_string_ptr(),item.outfile_name_.get_string_len()); }
  const ParseNode *list = node.children_[1];
  std::set<std::string> seen;
  for (int64_t i = 0; OB_SUCC(ret) && list != nullptr && i < list->num_child_; ++i) {
    const ParseNode *pair = list->children_[i];
    if (pair == nullptr || pair->num_child_ != 2 || pair->children_[0] == nullptr) { ret = OB_INVALID_ARGUMENT; break; }
    const ParseNode *key = pair->children_[0], *value = pair->children_[1];
    ObString name(key->str_len_,key->str_value_);
    std::string lower(name.ptr(),name.length());
    for (char &c : lower) { if (c >= 'A' && c <= 'Z') { c += 'a'-'A'; } }
    if (!seen.insert(lower).second) { ret = OB_INVALID_ARGUMENT; break; }
    ObObj scalar;
    if (value != nullptr && value->type_ == T_IDENT) { scalar.set_varchar(ObString(value->str_len_,value->str_value_)); }
    else if (value != nullptr && OB_FAIL(constant(*value,scalar))) { break; }
    if (lower == "header") {
      if (value == nullptr) { options.header = true; }
      else if (scalar.is_integer_type() && (scalar.get_int() == 0 || scalar.get_int() == 1)) { options.header = scalar.get_int() != 0; }
      else if (scalar.is_string_type() && scalar.get_string().case_compare("true") == 0) { options.header = true; }
      else if (scalar.is_string_type() && scalar.get_string().case_compare("false") == 0) { options.header = false; }
      else { ret = OB_INVALID_ARGUMENT; }
    } else if (!scalar.is_string_type()) { ret = OB_INVALID_ARGUMENT; }
    else {
      std::string text(scalar.get_string_ptr(),scalar.get_string_len());
      if (lower == "format") {
        if (scalar.get_string().case_compare("csv") != 0) { ret = OB_NOT_SUPPORTED; }
      } else if (lower == "nullstr" || lower == "null_string") { options.null_text = text; }
      else if (lower == "delimiter" || lower == "delim" || lower == "sep" || lower == "quote" || lower == "escape") {
        if (text.size() > 1 || (!text.empty() && (text[0] == 0 || text[0] == '\r' || text[0] == '\n'))) { ret = OB_INVALID_ARGUMENT; }
        else if (lower == "quote") { options.quote = text.empty() ? 0 : text[0]; }
        else if (lower == "escape") { options.escape = text.empty() ? 0 : text[0]; }
        else if (text.empty()) { ret = OB_INVALID_ARGUMENT; }
        else { options.delimiter = text[0]; }
      } else { ret = OB_INVALID_ARGUMENT; }
    }
  }
  if (OB_SUCC(ret) && options.quote != 0 && options.quote == options.delimiter) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; i < get_select_stmt()->get_select_item_size(); ++i) {
    const auto &selected = get_select_stmt()->get_select_item(i);
    FileColumn column; column.name.assign(selected.alias_name_.ptr(),selected.alias_name_.length());
    options.columns.push_back(column);
  }
  if (OB_SUCC(ret)) {
    item.is_file_copy_ = true;
    item.is_single_ = true;
    ret = options.encode(*allocator_,item.external_properties_);
    ObQueryHint &hint = get_stmt()->get_query_ctx()->get_query_hint_for_update();
    const_cast<ObGlobalHint &>(hint.get_global_hint()).plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
  }
  return ret;
}
} }

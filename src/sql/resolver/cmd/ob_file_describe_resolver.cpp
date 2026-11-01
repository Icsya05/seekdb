/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/cmd/ob_show_resolver.h"
#include "sql/engine/basic/ob_file_scan.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace sql {
using namespace common;

int ObShowResolver::resolve_describe_select(const ParseNode &select)
{
  int ret = OB_SUCCESS;
  ObSelectResolver query(params_);
  ObSelectStmt *described = nullptr;
  ObSqlString sql;
  if (OB_FAIL(query.resolve(select))) { }
  else if ((described = query.get_select_stmt()) == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (described->get_select_item_size() == 0 || described->get_select_item_size() > FileScanReader::MAX_COLUMNS) {
    ret = OB_SIZE_OVERFLOW;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < described->get_select_item_size(); ++i) {
    const SelectItem &item = described->get_select_item(i);
    if (item.expr_ == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
    const char *type = "VARCHAR";
    switch (item.expr_->get_result_type().get_type()) {
      case ObNullType: type = "NULL"; break;
      case ObTinyIntType: type = "BOOLEAN"; break;
      case ObIntType: case ObUInt64Type: type = "BIGINT"; break;
      case ObDoubleType: case ObFloatType: case ObNumberType: type = "DOUBLE"; break;
      case ObDateType: type = "DATE"; break;
      case ObDateTimeType: case ObTimestampType: type = "DATETIME"; break;
      default: break;
    }
    if (i != 0) { ret = sql.append(" UNION ALL "); }
    // Hex literals preserve arbitrary UTF-8 column names without depending on
    // NO_BACKSLASH_ESCAPES, ANSI_QUOTES, or SQL string escaping rules.
    if (OB_SUCC(ret)) { ret = sql.append("SELECT CONVERT(X'"); }
    for (int64_t j = 0; OB_SUCC(ret) && j < item.alias_name_.length(); ++j) {
      ret = sql.append_fmt("%02x", static_cast<unsigned char>(item.alias_name_.ptr()[j]));
    }
    if (OB_SUCC(ret)) {
      ret = sql.append_fmt("' USING utf8mb4) AS column_name, '%s' AS column_type, '%s' AS `null`, "
                          "NULL AS `key`, NULL AS `default`, NULL AS extra",
                          type, item.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG) ? "NO" : "YES");
    }
  }
  if (OB_SUCC(ret)) { ret = parse_and_resolve_select_sql(sql.string()); }
  return ret;
}
} }

/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/ai_column_expression.h"
#include "share/schema/ob_ai_column_schema.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int ai_catalog_identifier(ObSqlString &out, const ObString &name)
{
  int ret = out.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name[i] == '`' ? out.append("``") : out.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = out.append("`"); }
  return ret;
}
int ai_catalog_literal(ObSqlString &out, const ObString &value)
{
  int ret = out.append("CONVERT(X'");
  static const char digits[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const uint8_t b = static_cast<uint8_t>(value[i]);
    const char h[] = {digits[b >> 4], digits[b & 15]};
    ret = out.append(h, 2);
  }
  if (OB_SUCC(ret)) { ret = out.append("' USING utf8mb4)"); }
  return ret;
}
int ai_catalog_rowkey(const ObTableSchema &table, ObSqlString &out)
{
  int ret = out.assign("CONCAT(");
  ObSEArray<uint64_t, 4> keys;
  if (OB_SUCC(ret)) { ret = table.get_rowkey_column_ids(keys); }
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    const ObColumnSchemaV2 *key = table.get_column_schema(keys.at(i));
    ObSqlString label, identifier;
    if (key == nullptr) { ret = OB_SCHEMA_ERROR; }
    else if (OB_FAIL(ai_catalog_identifier(identifier, key->get_column_name_str()))) {
    } else if (OB_FAIL(label.assign(i == 0 ? "" : " AND "))) {
    } else if (OB_FAIL(label.append(identifier.string()))) {
    } else if (OB_FAIL(label.append(" = "))) {
    } else if (i != 0 && OB_FAIL(out.append(","))) {
    } else if (OB_FAIL(ai_catalog_literal(out, label.string()))) {
    } else if (OB_FAIL(out.append_fmt(",%s(CAST(%s AS CHAR))",
                  ob_is_numeric_type(key->get_data_type()) ? "" : "QUOTE", identifier.ptr()))) {
    }
  }
  if (OB_SUCC(ret)) { ret = out.append(")"); }
  return ret;
}
} // namespace

int ObDMLResolver::resolve_ai_column_catalog(const ObString &database, const ObString &name,
                                            const ObString &alias, TableItem *&item)
{
  int ret = OB_SUCCESS;
  const bool tasks = name.case_compare("__all_ai_column_task") == 0;
  ObSchemaGetterGuard *guard = schema_checker_->get_schema_guard();
  ObSEArray<uint64_t, 64> ids;
  ObSqlString query;
  if (!get_stmt()->is_select_stmt()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "writing AI column catalogs");
  } else if (guard == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(guard->get_table_ids_in_runtime(ids))) {
  }
  // These are views of the persistent schema and native MVCC row queue. In
  // particular, task scans run in the caller's transaction, without a second
  // connection or a stale in-memory mirror of uncommitted work.
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    const ObTableSchema *table = nullptr;
    const ObDatabaseSchema *db = nullptr;
    if (OB_FAIL(guard->get_table_schema(ids.at(i), table))) { break; }
    if (table == nullptr || !table->is_user_table() || table->is_in_recyclebin()) { continue; }
    for (int64_t j = 0; OB_SUCC(ret) && j < table->get_column_count(); ++j) {
      const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(j);
      if (!column->is_ai_column() || column->is_unused()) { continue; }
      ObSqlString field[AiColumnState::FIELD_COUNT], qualified, expression, rowkey;
      uint64_t field_id[AiColumnState::FIELD_COUNT] = {};
      for (int f = 0; OB_SUCC(ret) && f < AiColumnState::FIELD_COUNT; ++f) {
        ObSqlString raw;
        const ObColumnSchemaV2 *hidden = nullptr;
        if (OB_FAIL(AiColumnState::name(column->get_column_id(), static_cast<AiColumnState::Field>(f), raw))) {
        } else if ((hidden = table->get_column_schema(raw.string())) == nullptr) { ret = OB_SCHEMA_ERROR; }
        else if (OB_FAIL(ai_catalog_identifier(field[f], raw.string()))) {
        } else { field_id[f] = hidden->get_column_id(); }
      }
      if (OB_FAIL(ret)) {
      } else if (!query.empty() && OB_FAIL(query.append(" UNION ALL "))) {
      } else if (OB_FAIL(query.append_fmt("SELECT %lu AS table_id,%lu AS column_id,",
                                        table->get_table_id(), column->get_column_id()))) {
      } else if (OB_FAIL(ai_catalog_literal(query, column->get_column_name_str()))) {
      } else if (OB_FAIL(query.append(" AS column_name,"))) {
      } else if (!tasks) {
        if (OB_FAIL(AiColumnExpression::normalize(*column, *table, *guard, *allocator_, expression))) {
        } else if (OB_FAIL(query.append_fmt("%ld AS max_retries,%lu AS state_column_id,"
                     "%lu AS request_column_id,%lu AS result_spec_column_id,",
                     column->get_ai_column_max_retries(), field_id[AiColumnState::STATUS],
                     field_id[AiColumnState::REQUEST], field_id[AiColumnState::VERSION]))) {
        } else if (OB_FAIL(ai_catalog_literal(query, expression.string()))) {
        } else if (OB_FAIL(query.append_fmt(" AS normalized_expr,%ld AS definition_version,'async' AS sync_mode",
                                           column->get_ai_column_version()))) {
        }
      } else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), db))) {
      } else if (db == nullptr) { ret = OB_SCHEMA_ERROR; }
      else if (OB_FAIL(ai_catalog_identifier(qualified, db->get_database_name_str()))) {
      } else if (OB_FAIL(qualified.append("."))) {
      } else if (OB_FAIL(ai_catalog_identifier(qualified, table->get_table_name_str()))) {
      } else if (OB_FAIL(ai_catalog_rowkey(*table, rowkey))) {
      } else if (OB_FAIL(query.append_fmt("%s AS request_id,%s AS rowkey_predicate,"
                  "FROM_UNIXTIME(%s/1000000.0) AS eligible_ts,%s AS state,%s AS retry_count,"
                  "%s AS owner,%s AS lease_expiry,%s AS attempts FROM %s "
                  "WHERE %s='PENDING' OR %s<>%ld",
                  field[AiColumnState::REQUEST].ptr(), rowkey.ptr(), field[AiColumnState::ELIGIBLE].ptr(),
                  field[AiColumnState::TASK_STATE].ptr(), field[AiColumnState::RETRIES].ptr(),
                  field[AiColumnState::OWNER].ptr(), field[AiColumnState::LEASE].ptr(),
                  field[AiColumnState::ATTEMPTS].ptr(), qualified.ptr(),
                  field[AiColumnState::STATUS].ptr(), field[AiColumnState::VERSION].ptr(),
                  column->get_ai_column_version()))) {
      }
    }
  }
  if (OB_SUCC(ret) && query.empty()) {
    ret = query.assign(tasks
        ? "SELECT 0 AS table_id,0 AS column_id,CAST('' AS CHAR(256)) AS column_name,"
          "CAST('' AS CHAR(64)) AS request_id,CAST('' AS CHAR(65535)) AS rowkey_predicate,"
          "CAST(NULL AS DATETIME(6)) AS eligible_ts,0 AS state,0 AS retry_count,"
          "CAST('' AS CHAR(64)) AS owner,0 AS lease_expiry,0 AS attempts WHERE 0"
        : "SELECT 0 AS table_id,0 AS column_id,CAST('' AS CHAR(256)) AS column_name,"
          "0 AS max_retries,0 AS state_column_id,0 AS request_column_id,0 AS result_spec_column_id,"
          "CAST('' AS CHAR(65535)) AS normalized_expr,0 AS definition_version,'async' AS sync_mode WHERE 0");
  }
  ObParser parser(*allocator_, session_info_->get_sql_mode(), session_info_->get_charsets4parser());
  ParseResult parsed;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parser.parse(query.string(), parsed))) {
  } else {
    ObResolverParams child_params(params_);
    child_params.is_resolving_ai_catalog_ = true;
    ObSelectResolver resolver(child_params);
    resolver.set_current_level(current_level_);
    resolver.set_current_view_level(current_view_level_);
    if (OB_FAIL(resolver.resolve_child_stmt(*parsed.result_tree_->children_[0]))) {
    } else if (OB_FAIL(resolve_generate_table_item(resolver.get_child_stmt(), alias.empty() ? name : alias, item))) {
    } else {
      item->table_name_ = name;
      item->database_name_ = database;
      item->is_system_table_ = true;
      // The set of contributing tables changes on CREATE/DROP/ALTER. Build the
      // expansion on each execution, including prepared statement executions.
      ObQueryHint &hint = get_stmt()->get_query_ctx()->get_query_hint_for_update();
      const_cast<ObGlobalHint &>(hint.get_global_hint()).plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
    }
  }
  return ret;
}
} }

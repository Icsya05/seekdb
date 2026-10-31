#define USING_LOG_PREFIX SERVER_OMT
#include "observer/ai_service/ai_column_worker.h"
#include "sql/resolver/ai_column_expression.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "share/ob_server_struct.h"
#include "share/ob_share_util.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "share/object/ob_obj_cast.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace share;
using namespace share::schema;
using namespace sql;
namespace
{
constexpr int64_t LEASE_US = 60L * 1000 * 1000;
constexpr int64_t MODEL_TIMEOUT_US = 30L * 1000 * 1000;
constexpr int64_t MAX_RESULT_BYTES = 1024 * 1024;

struct ColumnTask
{
  const ObColumnSchemaV2 *column_ = nullptr;
  ObSqlString field_[AiColumnState::FIELD_COUNT];
  ObSqlString request_;
  ObSqlString expression_;
  ObSqlString value_;
  int64_t attempts_ = 0;
  bool selected_ = false;
  bool ready_ = false;
};

int identifier(ObSqlString &out, const ObString &name)
{
  int ret = out.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '`') { ret = out.append("``"); }
    else { ret = out.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = out.append("`"); }
  return ret;
}

// Hex literals cannot escape into SQL, including with NO_BACKSLASH_ESCAPES.
int literal(ObSqlString &out, const ObString &value)
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

int write_sql(const ObSqlString &query, int64_t &affected)
{
  int ret = OB_SUCCESS;
  ObTimeoutCtx timeout;
  if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, 3L * 1000 * 1000))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(query.ptr(), affected))) {
  }
  return ret;
}


int validate_result(const ObColumnSchemaV2 &column, const ObObj &value,
                    ObIAllocator &allocator, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  if (value.is_null()) {
    ret = out.assign("NULL"); // A successful NULL is READY, not a missing task.
  } else if (!value.is_string_type() || value.get_string().length() > MAX_RESULT_BYTES) {
    ret = OB_SIZE_OVERFLOW;
  } else if (column.is_collection() || column.is_json()) {
    ObSqlString encoded, query;
    int64_t dimension = 0;
    if (OB_FAIL(literal(encoded, value.get_string()))) {
    } else if (column.is_collection() && OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                   column.get_extended_type_info(), dimension))) {
    } else if (column.is_collection()) {
      ret = out.assign(encoded);
    } else {
      ret = out.assign_fmt("CAST(%s AS JSON)", encoded.ptr());
    }
    // Validate before publishing any member of the group. Native casts enforce
    // JSON grammar and the VECTOR shape; they never call a model here.
    if (OB_SUCC(ret)) {
      ret = column.is_collection() ? query.assign_fmt("SELECT CAST(VECTOR_DIMS(%s) AS SIGNED)", out.ptr())
                                   : query.assign_fmt("SELECT %s", out.ptr());
    }
    if (OB_SUCC(ret)) {
      ObTimeoutCtx timeout;
      ObMySQLProxy::MySQLResult result;
      if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, 3L * 1000 * 1000))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->read(result, query.ptr()))) {
      } else if (OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(result.get_result()->next())) {
      } else if (column.is_collection()) {
        int64_t actual_dimension = 0;
        if (OB_FAIL(result.get_result()->get_int(0L, actual_dimension))) {
        } else if (actual_dimension != dimension) { ret = OB_INVALID_ARGUMENT; }
      }
    }
  } else {
    ObDataTypeCastParams params;
    ObCastCtx cast(&allocator, &params, CM_NONE, column.get_collation_type());
    ObObj converted, checked;
    const ObObj *target = nullptr;
    // Text values are sent as text literals; numeric conversion must not silently
    // turn invalid model output into zero as a non-strict SQL SELECT CAST can.
    const ObObjType type = ob_is_text_tc(column.get_data_type()) ? ObVarcharType : column.get_data_type();
    if (OB_FAIL(ObObjCaster::to_type(type, cast, value, converted, target))) {
    } else if (OB_ISNULL(target)) { ret = OB_ERR_UNEXPECTED; }
    else if (!ob_is_text_tc(column.get_data_type()) && OB_FAIL(obj_accuracy_check(cast,
                 column.get_accuracy(), column.get_collation_type(), *target, checked, target))) {
    } else if (OB_FAIL(literal(out, value.get_string()))) {
    }
  }
  return ret;
}
} // namespace

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumn");
  while (!has_set_stop()) {
    bool did_work = false;
    ObSchemaGetterGuard guard;
    ObSEArray<uint64_t, 64> ids;
    int ret = OB_SUCCESS;
    if (GCTX.schema_service_ == nullptr || GCTX.sql_proxy_ == nullptr) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_ids_in_runtime(ids))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < ids.count(); ++i) {
      const ObTableSchema *table = nullptr;
      if (OB_FAIL(guard.get_table_schema(ids.at(i), table))) { break; }
      if (table == nullptr || !table->is_user_table() || table->is_in_recyclebin()) { continue; }
      bool has_ai = false;
      for (int64_t j = 0; j < table->get_column_count(); ++j) {
        has_ai |= table->get_column_schema_by_idx(j)->is_ai_column();
      }
      if (has_ai) {
        const int task_ret = process_table(table->get_table_id(), did_work);
        if (task_ret != OB_SUCCESS && task_ret != OB_ITER_END && REACH_TIME_INTERVAL(10L * 1000 * 1000)) {
          LOG_WARN("AI column scheduling failed", K(task_ret), "table_id", table->get_table_id());
        }
      }
    }
    if (!did_work) { ob_usleep(100 * 1000); }
  }
}

int AiColumnWorker::process_table(uint64_t table_id, bool &did_work)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSEArray<uint64_t, 4> keys;
  ObSEArray<ColumnTask *, 4> tasks;
  ObSqlString table_name, select, eligible, key_where, where, owner, claim, publish;
  const int64_t now = ObTimeUtility::current_time();
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (OB_ISNULL(table)) { ret = OB_TABLE_NOT_EXIST; }
  else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
  } else if (OB_ISNULL(database)) { ret = OB_SCHEMA_ERROR; }
  else if (OB_FAIL(identifier(table_name, database->get_database_name_str()))) {
  } else if (OB_FAIL(table_name.append("."))) {
  } else if (OB_FAIL(identifier(table_name, table->get_table_name_str()))) {
  } else if (OB_FAIL(table->get_rowkey_column_ids(keys))) {
  } else if (OB_FAIL(select.assign("SELECT UUID()"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    if (OB_FAIL(select.append(","))) {
    } else if (OB_FAIL(identifier(select, table->get_column_schema(keys.at(i))->get_column_name_str()))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (!column->is_ai_column() || column->is_unused()) { continue; }
    ColumnTask *task = OB_NEWx(ColumnTask, &allocator);
    if (OB_ISNULL(task)) { ret = OB_ALLOCATE_MEMORY_FAILED; break; }
    if (OB_FAIL(tasks.push_back(task))) { task->~ColumnTask(); break; }
    task->column_ = column;
    for (int f = 0; OB_SUCC(ret) && f < AiColumnState::FIELD_COUNT; ++f) {
      ObSqlString name;
      if (OB_FAIL(AiColumnState::name(column->get_column_id(), static_cast<AiColumnState::Field>(f), name))) {
      } else if (table->get_column_schema(name.string()) == nullptr) { ret = OB_SCHEMA_ERROR; }
      else if (OB_FAIL(identifier(task->field_[f], name.string()))) {
      } else if (OB_FAIL(select.append_fmt(",%s", task->field_[f].ptr()))) {
      }
    }
    if (OB_SUCC(ret)) {
      ret = eligible.append_fmt("%s((%s='PENDING' OR %s<>%ld) AND %s<%ld AND %s<=%ld)",
          eligible.empty() ? "" : " OR ", task->field_[AiColumnState::STATUS].ptr(),
          task->field_[AiColumnState::VERSION].ptr(), column->get_ai_column_version(),
          task->field_[AiColumnState::LEASE].ptr(), now,
          task->field_[AiColumnState::ELIGIBLE].ptr(), now);
    }
  }
  if (OB_SUCC(ret) && tasks.empty()) { ret = OB_ITER_END; }
  if (OB_SUCC(ret)) { ret = select.append_fmt(" FROM %s WHERE %s LIMIT 1", table_name.ptr(), eligible.ptr()); }
  // Close the read before claiming the row. This snapshot can see only committed
  // business rows, and no database transaction spans an HTTP request.
  if (OB_SUCC(ret)) {
    ObTimeoutCtx timeout;
    ObMySQLProxy::MySQLResult rows;
    sqlclient::ObMySQLResult *row = nullptr;
    ObString uuid;
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, 3L * 1000 * 1000))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->read(rows, select.ptr()))) {
    } else if (OB_ISNULL(row = rows.get_result())) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(row->next())) {
    } else if (OB_FAIL(row->get_varchar(0L, uuid))) {
    } else if (OB_FAIL(literal(owner, uuid))) {
    }
    char *buffer = static_cast<char *>(allocator.alloc(64 * 1024));
    if (OB_SUCC(ret) && buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
      ObObj value;
      int64_t length = 0;
      if (OB_FAIL(row->get_obj(i + 1, value))) {
      } else if (OB_FAIL(value.print_sql_literal(buffer, 64 * 1024, length))) {
      } else if (OB_FAIL(key_where.append(i == 0 ? "" : " AND "))) {
      } else if (OB_FAIL(identifier(key_where, table->get_column_schema(keys.at(i))->get_column_name_str()))) {
      } else if (OB_FAIL(key_where.append_fmt(" <=> %.*s", static_cast<int>(length), buffer))) {
      }
    }
    if (OB_SUCC(ret)) { ret = where.assign(key_where); }
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      ColumnTask &t = *tasks.at(i);
      const int64_t offset = 1 + keys.count() + i * AiColumnState::FIELD_COUNT;
      ObString status, request;
      int64_t lease = 0, version = 0, eligible_time = 0;
      if (OB_FAIL(row->get_varchar(offset, status))) {
      } else if (OB_FAIL(row->get_varchar(offset + 1, request))) {
      } else if (OB_FAIL(row->get_int(offset + AiColumnState::LEASE, lease))) {
      } else if (OB_FAIL(row->get_int(offset + AiColumnState::ATTEMPTS, t.attempts_))) {
      } else if (OB_FAIL(row->get_int(offset + AiColumnState::VERSION, version))) {
      } else if (OB_FAIL(row->get_int(offset + AiColumnState::ELIGIBLE, eligible_time))) {
      } else if (lease < now && eligible_time <= now && (status == "PENDING" || version != t.column_->get_ai_column_version())) {
        t.selected_ = true;
        if (version != t.column_->get_ai_column_version()) { t.attempts_ = 0; }
        if (OB_FAIL(literal(t.request_, request))) {
        } else if (OB_FAIL(claim.append_fmt("%s%s=%s,%s=%ld,%s=%ld,%s=%ld,%s='PENDING',",
                    claim.empty() ? "" : ",", t.field_[AiColumnState::OWNER].ptr(), owner.ptr(),
                    t.field_[AiColumnState::LEASE].ptr(), now + LEASE_US,
                    t.field_[AiColumnState::VERSION].ptr(), t.column_->get_ai_column_version(),
                    t.field_[AiColumnState::ATTEMPTS].ptr(), t.attempts_, t.field_[AiColumnState::STATUS].ptr()))) {
        } else if (OB_FAIL(identifier(claim, t.column_->get_column_name_str()))) {
        } else if (OB_FAIL(claim.append("=NULL"))) {
        } else if (OB_FAIL(where.append_fmt(" AND %s=%s AND %s<%ld AND %s=%ld AND %s=",
                    t.field_[AiColumnState::REQUEST].ptr(), t.request_.ptr(),
                    t.field_[AiColumnState::LEASE].ptr(), now,
                    t.field_[AiColumnState::VERSION].ptr(), version, t.field_[AiColumnState::STATUS].ptr()))) {
        } else if (OB_FAIL(literal(where, status))) {
        }
      }
    }
  }
  int64_t affected = 0;
  if (OB_SUCC(ret) && !claim.empty()) {
    ObSqlString query;
    if (OB_FAIL(query.assign_fmt("UPDATE %s SET %s WHERE %s", table_name.ptr(), claim.ptr(), where.ptr()))) {
    } else if (OB_FAIL(write_sql(query, affected))) {
    } else if (affected != 1) { ret = OB_ITER_END; }
    else { did_work = true; }
  }
  // Every subsequent write checks ownership, request identity and the current
  // definition in the same SQL statement, including across DROP/recreate.
  if (OB_SUCC(ret)) { ret = where.assign(key_where); }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    ColumnTask &t = *tasks.at(i);
    if (!t.selected_) { continue; }
    ret = where.append_fmt(" AND %s=%s AND %s=%s AND %s='PENDING' AND EXISTS "
        "(SELECT 1 FROM oceanbase.__all_column WHERE table_id=%lu AND column_id=%lu AND ai_column_version=%ld)",
        t.field_[AiColumnState::REQUEST].ptr(), t.request_.ptr(), t.field_[AiColumnState::OWNER].ptr(), owner.ptr(),
        t.field_[AiColumnState::STATUS].ptr(), table_id, t.column_->get_column_id(), t.column_->get_ai_column_version());
  }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
    ColumnTask &t = *tasks.at(i);
    if (!t.selected_) { continue; }
    const int expr_ret = AiColumnExpression::normalize(*t.column_, *table, guard, allocator, t.expression_);
    if (expr_ret != OB_SUCCESS) { t.attempts_ = t.column_->get_ai_column_max_retries() + 1; }
    while (OB_SUCC(ret) && !has_set_stop() && !t.ready_ && t.attempts_ <= t.column_->get_ai_column_max_retries()) {
      ObSqlString renew, query;
      for (int64_t j = 0; OB_SUCC(ret) && j < tasks.count(); ++j) {
        if (!tasks.at(j)->selected_) { continue; }
        ret = renew.append_fmt("%s%s=%ld", renew.empty() ? "" : ",",
            tasks.at(j)->field_[AiColumnState::LEASE].ptr(), ObTimeUtility::current_time() + LEASE_US);
      }
      ++t.attempts_;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(query.assign_fmt("UPDATE %s SET %s,%s=%ld,%s=2 WHERE %s", table_name.ptr(), renew.ptr(),
                   t.field_[AiColumnState::ATTEMPTS].ptr(), t.attempts_,
                   t.field_[AiColumnState::TASK_STATE].ptr(), where.ptr()))) {
      } else if (OB_FAIL(write_sql(query, affected))) {
      } else if (affected != 1) { ret = OB_ITER_END; }
      else if (OB_FAIL(query.assign_fmt("SELECT CAST(%s AS CHAR) FROM %s WHERE %s",
                   t.expression_.ptr(), table_name.ptr(), where.ptr()))) {
      } else {
        int call_ret = OB_SUCCESS;
        AiSingleAttemptGuard single_attempt;
        ObTimeoutCtx timeout;
        ObMySQLProxy::MySQLResult response;
        ObObj value;
        if (OB_SUCCESS != (call_ret = ObShareUtil::set_default_timeout_ctx(timeout, MODEL_TIMEOUT_US))) {
        } else if (OB_SUCCESS != (call_ret = GCTX.sql_proxy_->read(response, query.ptr()))) {
        } else if (response.get_result() == nullptr) { call_ret = OB_ERR_UNEXPECTED; }
        else if (OB_SUCCESS != (call_ret = response.get_result()->next())) {
          if (call_ret == OB_ITER_END) { ret = OB_ITER_END; }
        } else if (OB_SUCCESS != (call_ret = response.get_result()->get_obj(0L, value))) {
        } else {
          t.value_.reuse();
          call_ret = validate_result(*t.column_, value, allocator, t.value_);
        }
        t.ready_ = call_ret == OB_SUCCESS;
      }
      if (!t.ready_ && OB_SUCC(ret) && t.attempts_ <= t.column_->get_ai_column_max_retries()) {
        const int64_t eligible_time = ObTimeUtility::current_time() + 1000 * 1000;
        if (OB_FAIL(query.assign_fmt("UPDATE %s SET %s=3,%s=%ld,%s=%ld WHERE %s", table_name.ptr(),
                       t.field_[AiColumnState::TASK_STATE].ptr(), t.field_[AiColumnState::RETRIES].ptr(),
                       t.attempts_, t.field_[AiColumnState::ELIGIBLE].ptr(), eligible_time, where.ptr()))) {
        } else if (OB_FAIL(write_sql(query, affected))) {
        } else if (affected != 1) { ret = OB_ITER_END; }
        while (OB_SUCC(ret) && !has_set_stop() && ObTimeUtility::current_time() < eligible_time) {
          ob_usleep(50 * 1000);
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(publish.append(publish.empty() ? "" : ","))) {
      } else if (OB_FAIL(identifier(publish, t.column_->get_column_name_str()))) {
      } else if (OB_FAIL(publish.append_fmt("=%s,%s='%s',%s='',%s=0", t.ready_ ? t.value_.ptr() : "NULL",
                   t.field_[AiColumnState::STATUS].ptr(), t.ready_ ? "READY" : "FAILED",
                   t.field_[AiColumnState::OWNER].ptr(), t.field_[AiColumnState::LEASE].ptr()))) {
      }
    }
  }
  if (OB_SUCC(ret) && !has_set_stop() && !publish.empty()) {
    ObSqlString query;
    if (OB_FAIL(query.assign_fmt("UPDATE %s SET %s WHERE %s", table_name.ptr(), publish.ptr(), where.ptr()))) {
    } else if (OB_FAIL(write_sql(query, affected))) {
    }
  }
  // Also release surviving members of a stale group. Their request predicate
  // prevents touching newer work; a crashed owner instead expires by its lease.
  if (did_work && !owner.empty()) {
    for (int64_t i = 0; i < tasks.count(); ++i) {
      ColumnTask &t = *tasks.at(i);
      if (!t.selected_) { continue; }
      ObSqlString release;
      if (OB_SUCCESS == release.assign_fmt("UPDATE %s SET %s='',%s=0 WHERE %s AND %s=%s AND %s=%s",
             table_name.ptr(), t.field_[AiColumnState::OWNER].ptr(), t.field_[AiColumnState::LEASE].ptr(),
             key_where.ptr(), t.field_[AiColumnState::REQUEST].ptr(), t.request_.ptr(),
             t.field_[AiColumnState::OWNER].ptr(), owner.ptr())) {
        int64_t ignored = 0;
        (void)write_sql(release, ignored);
      }
    }
  }
  for (int64_t i = 0; i < tasks.count(); ++i) { tasks.at(i)->~ColumnTask(); }
  return ret;
}
} // namespace observer
} // namespace oceanbase

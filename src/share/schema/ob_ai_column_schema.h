#pragma once

#include "share/schema/ob_column_schema.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
// The durable queue lives in the same physical row as its inputs. Inserting,
// invalidating or deleting it therefore uses the business transaction's MVCC.
class AiColumnState
{
public:
  enum Field { STATUS, REQUEST, OWNER, LEASE, ATTEMPTS, VERSION, ELIGIBLE, RETRIES, TASK_STATE, FIELD_COUNT };

  static bool depends_on(const ObColumnSchemaV2 &column, uint64_t id)
  {
    const common::ObString &deps = column.get_ai_column_deps();
    uint64_t value = 0;
    for (int64_t i = 0; i <= deps.length(); ++i) {
      if (i == deps.length() || deps[i] == ',') {
        if (value == id) { return true; }
        value = 0;
      } else if (deps[i] >= '0' && deps[i] <= '9') {
        value = value * 10 + (deps[i] - '0');
      }
    }
    return false;
  }

  static int name(uint64_t column_id, Field field, common::ObSqlString &out)
  {
    static const char *suffix[] = {"status", "request", "owner", "lease", "attempts", "version", "eligible", "retries", "task_state"};
    int ret = common::OB_SUCCESS;
    if (field < STATUS || field >= FIELD_COUNT) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      ret = out.assign_fmt("__ai_%lu_%s", column_id, suffix[field]);
    }
    return ret;
  }

  static int build(const ObColumnSchemaV2 &ai_column, Field field,
                   uint64_t column_id, ObColumnSchemaV2 &out)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObSqlString column_name;
    ObObj value;
    out.reset();
    out.set_column_id(column_id);
    out.set_table_id(ai_column.get_table_id());
    out.set_is_hidden(true);
    out.set_nullable(false);
    out.add_column_flag(AI_COLUMN_STATE_FLAG);
    if (OB_FAIL(name(ai_column.get_column_id(), field, column_name))) {
    } else if (OB_FAIL(out.set_column_name(column_name.string()))) {
    } else {
      if (field >= LEASE) {
        out.set_data_type(ObIntType);
        out.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType]);
        value.set_int(field == TASK_STATE ? 1 : 0);
      } else {
        out.set_data_type(ObVarcharType);
        out.set_data_length(64);
        out.set_charset_type(CHARSET_UTF8MB4);
        out.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        value.set_varchar(field == STATUS ? "PENDING" : "");
        value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
      if (OB_FAIL(out.set_orig_default_value(value))) {
      } else if (OB_FAIL(out.set_cur_default_value(value, false))) {
      }
      if (OB_SUCC(ret) && field == ELIGIBLE) {
        value.set_varchar("CAST(UNIX_TIMESTAMP(NOW(6))*1000000 AS SIGNED)");
        ret = out.set_cur_default_value(value, true);
      }
      if (OB_SUCC(ret) && field == REQUEST) {
        // New inserts receive a different identity even after DELETE/reinsert
        // of the same primary key. ALTER's original default is materialized once.
        value.set_varchar("uuid()");
        if (OB_FAIL(out.set_cur_default_value(value, true))) {
        }
      }
    }
    return ret;
  }
};
} // namespace schema
} // namespace share
} // namespace oceanbase

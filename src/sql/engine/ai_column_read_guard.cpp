#define USING_LOG_PREFIX SQL
#include "sql/engine/ai_column_read_guard.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace
{
class AiReadCollector final : public ObStmtExprVisitor
{
public:
  AiReadCollector(ObIArray<uint64_t> &refs, ObIArray<ObDMLStmt *> &parents,
                  bool projection) : refs_(refs), parents_(parents)
  {
    remove_all();
    for (DmlStmtScope scope : {SCOPE_FROM, SCOPE_JOINED_TABLE, SCOPE_SEMI_INFO, SCOPE_WHERE,
         SCOPE_GROUPBY, SCOPE_HAVING, SCOPE_ORDERBY, SCOPE_LIMIT, SCOPE_QUALIFY_FILTER}) { add_scope(scope); }
    if (projection) { add_scope(SCOPE_SELECT); }
  }

  static int statement(ObDMLStmt &stmt, ObIArray<uint64_t> &refs,
                        ObIArray<ObDMLStmt *> &parents, bool projection)
  {
    int ret = OB_SUCCESS;
    // Recursive CTE back-edges do not introduce additional base-column references.
    for (int64_t i = 0; i < parents.count(); ++i) { if (parents.at(i) == &stmt) { return ret; } }
    if (OB_FAIL(parents.push_back(&stmt))) { return ret; }
    AiReadCollector visitor(refs, parents, projection);
    ret = stmt.iterate_stmt_expr(visitor);
    if (OB_SUCC(ret) && stmt.is_update_stmt()) {
      for (ObUpdateTableInfo *table : static_cast<ObUpdateStmt &>(stmt).get_update_table_info()) {
        for (ObAssignment &assignment : table->assignments_) {
          if (OB_SUCC(ret) && !assignment.is_implicit_) { ret = visitor.do_visit(assignment.expr_); }
        }
      }
    }
    for (TableItem *table : stmt.get_table_items()) {
      if (OB_SUCC(ret) && table->ref_query_ != nullptr) {
        ret = SMART_CALL(statement(*table->ref_query_, refs, parents, false));
      }
    }
    if (OB_SUCC(ret) && stmt.is_select_stmt()) {
      for (ObSelectStmt *branch : static_cast<ObSelectStmt &>(stmt).get_set_query()) {
        if (OB_SUCC(ret)) { ret = SMART_CALL(statement(*branch, refs, parents, projection)); }
      }
    }
    parents.pop_back();
    return ret;
  }

  int projection(ObSelectStmt &stmt, int64_t index)
  {
    int ret = OB_SUCCESS;
    if (index < 0 || index >= stmt.get_select_item_size()) { ret = OB_ERR_UNEXPECTED; }
    else if (!stmt.get_set_query().empty()) {
      for (ObSelectStmt *branch : stmt.get_set_query()) {
        if (OB_SUCC(ret)) { ret = SMART_CALL(projection(*branch, index)); }
      }
    } else {
      if (OB_FAIL(parents_.push_back(&stmt))) { return ret; }
      AiReadCollector child(refs_, parents_, false);
      ret = child.do_visit(stmt.get_select_item(index).expr_);
      parents_.pop_back();
    }
    return ret;
  }

  int do_visit(ObRawExpr *&expr) override
  {
    int ret = OB_SUCCESS;
    if (expr == nullptr) {
    } else if (expr->is_query_ref_expr()) {
      ObSelectStmt *child = static_cast<ObQueryRefRawExpr *>(expr)->get_ref_stmt();
      if (child != nullptr) { ret = SMART_CALL(statement(*child, refs_, parents_, true)); }
    } else if (expr->is_exec_param_expr()) {
      ret = SMART_CALL(do_visit(static_cast<ObExecParamRawExpr *>(expr)->get_ref_expr()));
    } else if (expr->is_column_ref_expr()) {
      ObColumnRefRawExpr &column = *static_cast<ObColumnRefRawExpr *>(expr);
      const TableItem *table = nullptr;
      for (int64_t i = parents_.count() - 1; table == nullptr && i >= 0; --i) {
        table = parents_.at(i)->get_table_item_by_id(column.get_table_id());
      }
      if (table != nullptr && table->ref_query_ != nullptr) {
        ret = SMART_CALL(projection(*table->ref_query_, column.get_column_id() - OB_APP_MIN_COLUMN_ID));
      } else if (table != nullptr && table->is_basic_table() && (column.get_column_flags() & AI_COLUMN_FLAG)) {
        bool duplicate = false;
        for (int64_t i = 0; i < refs_.count(); i += 2) {
          duplicate |= refs_.at(i) == table->ref_id_ && refs_.at(i + 1) == column.get_column_id();
        }
        if (!duplicate && OB_FAIL(refs_.push_back(table->ref_id_))) {
        } else if (!duplicate && OB_FAIL(refs_.push_back(column.get_column_id()))) {
        }
      } else if (column.get_dependant_expr() != nullptr) {
        ret = SMART_CALL(do_visit(column.get_dependant_expr()));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
        ret = SMART_CALL(do_visit(expr->get_param_expr(i)));
      }
    }
    return ret;
  }
private:
  ObIArray<uint64_t> &refs_;
  ObIArray<ObDMLStmt *> &parents_;
};

int quote_ai_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
} // namespace

int AiColumnReadGuard::collect(ObDMLStmt &stmt, ObIArray<uint64_t> &references)
{
  ObSEArray<ObDMLStmt *, 4> parents;
  return stmt.is_explain_stmt() ? OB_SUCCESS : AiReadCollector::statement(stmt, references, parents, true);
}

int AiColumnReadGuard::check(ObExecContext &context, const ObIArray<uint64_t> &references)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = context.get_my_session();
  bool strict = false;
  if (references.empty()) { return ret; }
  if (session == nullptr || context.get_sql_ctx() == nullptr || context.get_sql_ctx()->schema_guard_ == nullptr
      || references.count() % 2 != 0) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ, strict)) || !strict) { return ret; }
  ObSchemaGetterGuard &schema = *context.get_sql_ctx()->schema_guard_;
  sqlclient::ObISQLConnectionGuard owned_connection;
  ObIInnerSQLConnection *connection = as_inner_sql_connection(session->get_inner_conn());
  bool created = false;
  if (connection == nullptr) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, owned_connection))) {
    } else if (OB_ISNULL(connection = as_inner_sql_connection(owned_connection.get_ptr()))) { ret = OB_ERR_UNEXPECTED; }
    else { session->set_inner_conn(connection); created = true; }
  }
  // Share the statement's transaction and snapshot, including its own uncommitted
  // invalidations. The nested-session guard restores the caller on every exit.
  SMART_VAR(ObSQLSessionInfo::StmtSavedValue, saved_session) {
    ObIInnerSQLConnection::SavedValue saved_connection;
    bool nested = false;
    if (OB_SUCC(ret)) {
      if (OB_FAIL(connection->begin_nested_session(saved_session, saved_connection, true))) {
      } else { nested = true; }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < references.count(); i += 2) {
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      const ObColumnSchemaV2 *column = nullptr;
      ObSqlString query, status, version;
      if (OB_FAIL(schema.get_table_schema(references.at(i), table))) {
      } else if (table == nullptr || (column = table->get_column_schema(references.at(i + 1))) == nullptr) {
        ret = OB_SCHEMA_ERROR;
      } else if (OB_FAIL(schema.get_database_schema(table->get_database_id(), database))) {
      } else if (database == nullptr) { ret = OB_SCHEMA_ERROR; }
      else if (OB_FAIL(AiColumnState::name(column->get_column_id(), AiColumnState::STATUS, status))) {
      } else if (OB_FAIL(AiColumnState::name(column->get_column_id(), AiColumnState::VERSION, version))) {
      } else if (OB_FAIL(query.assign("SELECT 1 FROM "))) {
      } else if (OB_FAIL(quote_ai_identifier(query, database->get_database_name_str()))) {
      } else if (OB_FAIL(query.append("."))) {
      } else if (OB_FAIL(quote_ai_identifier(query, table->get_table_name_str()))) {
      } else if (OB_FAIL(query.append_fmt(" WHERE `%s`<>'READY' OR `%s`<>%ld LIMIT 1",
                     status.ptr(), version.ptr(), column->get_ai_column_version()))) {
      } else {
        ObMySQLProxy::MySQLResult result;
        if (OB_FAIL(connection->execute_read(query.string(), result))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_ITER_END == (ret = result.get_result()->next())) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) {
          ret = OB_AI_COLUMN_UNFINISHED;
          LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED, table->get_table_id(), column->get_column_id());
        }
      }
    }
    if (nested) {
      const int restore_ret = connection->end_nested_session(saved_session, saved_connection);
      if (OB_SUCC(ret)) { ret = restore_ret; }
    }
  }
  if (created) { session->set_inner_conn(nullptr); }
  return ret;
}
} // namespace sql
} // namespace oceanbase

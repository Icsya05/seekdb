/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ai_column_expression.h"
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

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
int print_argument(ObRawExpr *expr, ObSchemaGetterGuard &guard, ObIAllocator &allocator, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  constexpr int64_t capacity = 256 * 1024;
  char *buffer = static_cast<char *>(allocator.alloc(capacity));
  int64_t pos = 0;
  if (OB_ISNULL(buffer)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObRawExprPrinter printer(buffer, capacity, &pos, &guard);
    if (OB_FAIL(printer.do_print(expr, T_NONE_SCOPE))) {
    } else if (OB_FAIL(out.assign(buffer, pos))) {
    }
  }
  return ret;
}

int AiColumnExpression::normalize(const ObColumnSchemaV2 &column, const ObTableSchema &table,
                     ObSchemaGetterGuard &guard, ObIAllocator &allocator, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  ObRawExprFactory factory(allocator);
  ObRawExpr *expr = nullptr;
  const ParseNode *node = nullptr;
  ObSEArray<ObQualifiedName, 4> refs;
  SMART_VARS_3((ObSQLSessionInfo, session), (ObExecContext, context, allocator),
               (ObPhysicalPlanCtx, plan, allocator)) {
    LinkExecCtxGuard link(session, context);
    context.set_my_session(&session);
    context.set_physical_plan_ctx(&plan);
    if (OB_FAIL(session.init(0, &allocator))) {
    } else if (OB_FAIL(session.load_default_sys_variable(false, false))) {
    } else if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(column.get_ai_column_expr(),
                        session.get_charsets4parser(), allocator, node))) {
    } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(factory, session, *node,
                        expr, refs, nullptr, nullptr, nullptr, ObResolverUtils::DISABLE_CHECK))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
      const ObColumnSchemaV2 *dependency = table.get_column_schema(refs.at(i).col_name_);
      if (OB_ISNULL(dependency)) { ret = OB_SCHEMA_ERROR; }
      else { ret = ObRawExprUtils::init_column_expr(*dependency, &session, *refs.at(i).ref_expr_); }
    }
    ObSqlString model, prompt, options;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(expr) || expr->get_param_count() < 2) {
      ret = OB_SCHEMA_ERROR;
    } else if (OB_FAIL(print_argument(expr->get_param_expr(0), guard, allocator, model))) {
    } else if (OB_FAIL(print_argument(expr->get_param_expr(1), guard, allocator, prompt))) {
    } else if (column.is_collection()) {
      int64_t dimension = 0;
      if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension))) {
      } else if (OB_FAIL(out.assign_fmt("AI_EMBED(%s,%s,%ld)", model.ptr(), prompt.ptr(), dimension))) {
      }
    } else {
      const char *instruction = column.is_json() ? "Return exactly one valid JSON value, without markdown or explanation."
          : ob_is_integer_type(column.get_data_type()) ? "Return exactly one base-10 integer, without markdown or explanation."
          : ob_is_numeric_type(column.get_data_type()) ? "Return exactly one decimal number, without markdown or explanation."
          : "Return only plain text, without markdown or explanation.";
      if (expr->get_param_count() == 3) {
        ret = print_argument(expr->get_param_expr(2), guard, allocator, options);
      }
      if (OB_SUCC(ret)) {
        ret = out.assign_fmt("AI_COMPLETE(%s,%s,JSON_SET(COALESCE(%s,JSON_OBJECT()),'$.system_prompt','%s'))",
            model.ptr(), prompt.ptr(), options.empty() ? "JSON_OBJECT()" : options.ptr(), instruction);
      }
    }
    context.set_physical_plan_ctx(nullptr);
  }
  return ret;
}

} }

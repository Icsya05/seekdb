/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "lib/string/ob_sql_string.h"
namespace oceanbase { namespace sql {
class AiColumnExpression {
public:
  static int normalize(const share::schema::ObColumnSchemaV2 &column,
                       const share::schema::ObTableSchema &table,
                       share::schema::ObSchemaGetterGuard &guard,
                       common::ObIAllocator &allocator, common::ObSqlString &out);
};
} }

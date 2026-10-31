/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/ob_file_scan.h"

namespace oceanbase { namespace sql {
// The descriptor is borrowed from FileScanReader after secure path validation.
// Only metadata, dictionaries and one page per projected column are retained.
class ParquetFileReader {
public:
  ParquetFileReader();
  ~ParquetFileReader();
  int open(FILE *file, FileScanDefinition &definition,
           const common::ObIArray<int64_t> *projection = nullptr);
  int next(common::ObIAllocator &allocator, common::ObIArray<common::ObObj> &values);
private:
  struct State;
  State *state_;
  ParquetFileReader(const ParquetFileReader &) = delete;
  ParquetFileReader &operator=(const ParquetFileReader &) = delete;
};
} }

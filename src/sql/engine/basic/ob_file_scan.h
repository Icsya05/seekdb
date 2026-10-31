/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "object/ob_object.h"

namespace oceanbase { namespace sql {

enum class FileValueType { UNKNOWN, BOOLEAN, BIGINT, DOUBLE, VARCHAR, DATE, DATETIME };

struct FileColumn {
  std::string name;
  FileValueType type = FileValueType::UNKNOWN;
  bool nullable = false;
  int16_t scale = 0;
};

struct FileIdentity {
  int64_t device = 0;
  int64_t inode = 0;
  int64_t size = 0;
  int64_t modified = 0;
  int64_t changed = 0;
  bool operator==(const FileIdentity &other) const;
};

struct FileScanDefinition {
  enum Format { CSV, JSONL, PARQUET, GLOB };
  Format format = CSV;
  std::string path;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
  FileIdentity identity;
  std::vector<FileColumn> columns;
  int64_t row_count = 0;

  int encode(common::ObIAllocator &allocator, common::ObString &result) const;
  int decode(const common::ObString &value);
  static common::ObObjType object_type(FileValueType type);
  static const char *type_name(FileValueType type);
};

// Owns a read-only descriptor. Only one bounded record and the schema are held
// in memory; inference and execution use the same parser.
class FileScanReader {
public:
  static constexpr int64_t MAX_RECORD_BYTES = 16 * 1024 * 1024;
  static constexpr int64_t MAX_COLUMNS = 1024;
  FileScanReader() = default;
  ~FileScanReader() { close(); }
  int infer(FileScanDefinition &definition, const common::ObString &secure_root);
  int open(const FileScanDefinition &definition, const common::ObString &secure_root);
  int next(common::ObIAllocator &allocator, common::ObIArray<common::ObObj> &values);
  int check_unchanged() const;
  void close();
private:
  struct Cell {
    std::string text;
    bool is_null = false;
    bool string_value = false;
  };
  int open_file(const std::string &path, const common::ObString &root);
  int discover(const std::string &pattern, const common::ObString &root);
  int csv_record(std::vector<Cell> &cells);
  int json_record(std::vector<Cell> &cells, std::vector<std::string> &names);
  int record(std::vector<Cell> &cells, std::vector<std::string> &names);
  int skip_header();
  int read_identity(FileIdentity &identity) const;
  int convert(const Cell &cell, const FileColumn &column,
              common::ObIAllocator &allocator, common::ObObj &value);
  static FileValueType classify(const Cell &cell, int16_t &scale);
  static FileValueType widen(FileValueType left, FileValueType right);
  FILE *file_ = nullptr;
  FileScanDefinition definition_;
  std::vector<std::string> files_;
  size_t file_index_ = 0;
  int64_t record_number_ = 0;
  FileIdentity opened_identity_;
  FileScanReader(const FileScanReader &) = delete;
  FileScanReader &operator=(const FileScanReader &) = delete;
};

} }

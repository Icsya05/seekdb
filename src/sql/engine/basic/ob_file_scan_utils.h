#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

enum class Format { CSV, JSONL };
enum class Type { NULL_VALUE, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
enum Status { OK = 0, END = 1, INVALID = -1, DENIED = -2, CHANGED = -3,
              IO_ERROR = -4, UNSUPPORTED = -5, LIMIT_EXCEEDED = -6, CANCELLED = -7 };

struct Options {
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
  int64_t sample_size = -1;
};
struct Column {
  std::string name;
  Type type = Type::NULL_VALUE;
  int64_t max_length = 0;
  bool nullable = false;
};
struct Cell {
  std::string text;
  bool is_null = false;
};
struct Fingerprint {
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t size = 0;
  int64_t mtime_ns = 0;
  int64_t ctime_ns = 0;
};
struct Binding {
  Format format = Format::CSV;
  Options options;
  std::string path;
  std::vector<Column> columns;
  Fingerprint fingerprint;
};

int infer_file(const std::string &path, const std::string &secure_dir,
               Format format, const Options &options, Binding &binding, std::string &error,
               const std::function<int()> &check_status = {});
int serialize_binding(const Binding &binding, std::string &json, std::string &error);
int deserialize_binding(const std::string &json, Binding &binding, std::string &error);

class Scanner {
public:
  Scanner();
  ~Scanner();
  int open(const Binding &binding, const std::string &secure_dir, std::string &error);
  int next(std::vector<Cell> &row, std::string &error,
           const std::function<int()> &check_status = {});
  int rescan(std::string &error);
  void close();
private:
  Scanner(const Scanner &) = delete;
  Scanner &operator=(const Scanner &) = delete;
  class Impl;
  Impl *impl_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase

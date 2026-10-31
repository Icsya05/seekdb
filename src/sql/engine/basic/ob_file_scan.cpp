/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan.h"
#include "sql/engine/basic/ob_parquet_file_reader.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#ifndef _WIN32
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_warning_buffer.h"
#include "lib/worker.h"
#include "share/ob_errno.h"

namespace oceanbase { namespace sql {
using namespace common;

namespace {
ObString view(const std::string &text) { return ObString(text.size(), text.data()); }
std::string copy_string(const ObIJsonBase *node)
{
  return node->get_data_length() == 0 ? std::string() : std::string(node->get_data(), node->get_data_length());
}
int file_error(const char *message)
{
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, message);
  return OB_INVALID_ARGUMENT;
}
int record_error(int ret, int64_t record)
{
  ObWarningBuffer *warnings = ob_get_tsi_warning_buffer();
  if (warnings != nullptr && ret != OB_SUCCESS && ret != OB_ITER_END) {
    std::string reason = warnings->get_err_code() == ret ? warnings->get_err_msg() : "invalid input or I/O error";
    if (reason.compare(0,16,"file SQL record ") != 0) {
      std::string message = "file SQL record " + std::to_string(record) + ": " + reason;
      warnings->set_error(message.c_str(),ret);
    }
  }
  return ret;
}
int valid_utf8(const std::string &text)
{
  int64_t length = 0;
  int32_t error = 0;
  int ret = ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, text.data(), text.size(), length, error);
  if (OB_SUCC(ret) && (error != 0 || length != static_cast<int64_t>(text.size()))) {
    ret = file_error("file SQL: invalid UTF-8 input");
  }
  return ret;
}
std::string lower_ascii(std::string text)
{
  for (char &c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return text;
}
bool integer(const std::string &text, int64_t &value)
{
  if (text.empty()) { return false; }
  size_t start = text[0] == '-' || text[0] == '+' ? 1 : 0;
  if (start == text.size()) { return false; }
  for (size_t i = start; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') { return false; }
  }
  errno = 0;
  char *end = nullptr;
  value = strtoll(text.c_str(), &end, 10);
  return errno != ERANGE && end == text.c_str() + text.size();
}
bool floating(const std::string &text, double &value)
{
  if (text.empty() || text.find_first_of(".eE") == std::string::npos) { return false; }
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E')) { return false; }
  }
  errno = 0;
  char *end = nullptr;
  value = strtod(text.c_str(), &end);
  return errno != ERANGE && end == text.c_str() + text.size() && std::isfinite(value);
}
bool calendar(const std::string &text, int16_t &scale)
{
  scale = 0;
  if (text.size() != 10 && (text.size() < 19 || text.size() > 26)) { return false; }
  if (text[4] != '-' || text[7] != '-') { return false; }
  auto digits = [&](size_t first, size_t count) {
    int value = 0;
    for (size_t i = first; i < first + count; ++i) {
      if (text[i] < '0' || text[i] > '9') { return -1; }
      value = value * 10 + text[i] - '0';
    }
    return value;
  };
  int year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
  static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (year < 1 || month < 1 || month > 12 || day < 1) { return false; }
  int max_day = days[month-1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (day > max_day) { return false; }
  if (text.size() > 10) {
    if ((text[10] != ' ' && text[10] != 'T') || text[13] != ':' || text[16] != ':') { return false; }
    int hour = digits(11,2), minute = digits(14,2), second = digits(17,2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) { return false; }
    if (text.size() > 19) {
      if (text[19] != '.' || text.size() == 20 || digits(20,text.size()-20) < 0) { return false; }
      scale = text.size() - 20;
    }
  }
  return true;
}
#ifndef _WIN32
int canonical(const std::string &path, const ObString &allowed, std::string &result)
{
  if (path.empty() || path.find('\0') != std::string::npos || path.size() >= PATH_MAX
      || allowed.empty() || allowed.case_compare("NULL") == 0) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  std::string root(allowed.ptr(), allowed.length());
  if (root.find('\0') != std::string::npos) { return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; }
  char root_path[PATH_MAX], target[PATH_MAX];
  struct stat st;
  if (realpath(root.c_str(), root_path) == nullptr || stat(root_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  if (realpath(path.c_str(), target) == nullptr) { return OB_FILE_NOT_EXIST; }
  size_t n = strlen(root_path);
  size_t m = strlen(target);
  if (m < n || memcmp(root_path, target, n) != 0 || (n != 1 && m > n && target[n] != '/')) {
    return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  }
  result = target;
  return OB_SUCCESS;
}
#endif
}

bool FileIdentity::operator==(const FileIdentity &other) const
{
  return device == other.device && inode == other.inode && size == other.size
      && modified == other.modified && changed == other.changed;
}

ObObjType FileScanDefinition::object_type(FileValueType type)
{
  switch (type) {
    case FileValueType::UNKNOWN: return ObNullType;
    case FileValueType::BOOLEAN: return ObTinyIntType;
    case FileValueType::BIGINT: return ObIntType;
    case FileValueType::DOUBLE: return ObDoubleType;
    case FileValueType::DATE: return ObDateType;
    case FileValueType::DATETIME: return ObDateTimeType;
    default: return ObVarcharType;
  }
}
const char *FileScanDefinition::type_name(FileValueType type)
{
  static const char *names[] = {"NULL","BOOLEAN","BIGINT","DOUBLE","VARCHAR","DATE","DATETIME"};
  return names[static_cast<int>(type)];
}

int FileScanDefinition::encode(ObIAllocator &allocator, ObString &result) const
{
  int ret = OB_SUCCESS;
  ObJsonArray array(&allocator);
  auto number = [&](int64_t n) {
    int ret = OB_SUCCESS;
    ObJsonInt *node = OB_NEWx(ObJsonInt, &allocator, n);
    if (OB_ISNULL(node)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = array.append(node); }
    return ret;
  };
  auto string = [&](const std::string &s) {
    int ret = OB_SUCCESS;
    ObJsonString *node = OB_NEWx(ObJsonString, &allocator, view(s));
    if (OB_ISNULL(node)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = array.append(node); }
    return ret;
  };
  if (OB_FAIL(number(1)) || OB_FAIL(number(format)) || OB_FAIL(string(path))
      || OB_FAIL(number(header)) || OB_FAIL(number(delimiter)) || OB_FAIL(number(quote))
      || OB_FAIL(number(escape)) || OB_FAIL(string(null_text)) || OB_FAIL(number(identity.device))
      || OB_FAIL(number(identity.inode)) || OB_FAIL(number(identity.size))
      || OB_FAIL(number(identity.modified)) || OB_FAIL(number(identity.changed))
      || OB_FAIL(number(row_count))) {
  }
  for (const FileColumn &column : columns) {
    if (OB_SUCC(ret) && (OB_FAIL(string(column.name)) || OB_FAIL(number(static_cast<int>(column.type)))
        || OB_FAIL(number(column.nullable)) || OB_FAIL(number(column.scale)))) { }
  }
  ObJsonBuffer buffer(&allocator);
  if (OB_SUCC(ret) && OB_SUCC(ret = array.print(buffer, true))) { ret = buffer.get_result_string(result); }
  return ret;
}

int FileScanDefinition::decode(const ObString &value)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("FileScanMeta");
  ObIJsonBase *base = nullptr;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, value, ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, base))) {
  } else if (base->json_type() != ObJsonNodeType::J_ARRAY || base->element_count() < 14
             || (base->element_count()-14) % 4 != 0 || (base->element_count()-14)/4 > FileScanReader::MAX_COLUMNS) {
    ret = OB_INVALID_DATA;
  } else {
    auto *array = static_cast<ObJsonArray *>(base);
    for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
      bool text = i == 2 || i == 7 || (i >= 14 && (i-14)%4 == 0);
      ObIJsonBase *node = array->get_value(i);
      if (text ? node->json_type() != ObJsonNodeType::J_STRING
               : (node->json_type() != ObJsonNodeType::J_INT
                  && (node->json_type() != ObJsonNodeType::J_UINT || node->get_uint() > INT64_MAX))) {
        ret = OB_INVALID_DATA;
      }
    }
    if (OB_SUCC(ret)) {
      auto n = [&](int i) { auto *node = array->get_value(i); return node->json_type() == ObJsonNodeType::J_UINT
                              ? static_cast<int64_t>(node->get_uint()) : node->get_int(); };
      if (n(0) != 1 || n(1) < CSV || n(1) > GLOB) { return OB_INVALID_DATA; }
      format = static_cast<Format>(n(1)); path = copy_string(array->get_value(2));
      header = n(3); delimiter = n(4); quote = n(5); escape = n(6);
      null_text = copy_string(array->get_value(7));
      identity = {n(8),n(9),n(10),n(11),n(12)}; row_count = n(13);
      columns.clear();
      for (int64_t i = 14; OB_SUCC(ret) && i < array->element_count(); i += 4) {
        if (n(i+1) < 0 || n(i+1) > static_cast<int>(FileValueType::DATETIME)) { ret = OB_INVALID_DATA; }
        else { columns.push_back({copy_string(array->get_value(i)), static_cast<FileValueType>(n(i+1)), n(i+2) != 0, static_cast<int16_t>(n(i+3))}); }
      }
    }
  }
  return ret;
}

FileScanReader::~FileScanReader() { close(); }
void FileScanReader::close()
{
  delete parquet_; parquet_ = nullptr;
  if (file_ != nullptr) { fclose(file_); file_ = nullptr; }
  files_.clear(); file_index_ = 0; record_number_ = 0;
}
int FileScanReader::read_identity(FileIdentity &identity) const
{
#ifdef _WIN32
  return OB_NOT_SUPPORTED;
#else
  struct stat st;
  if (file_ == nullptr || fstat(fileno(file_), &st) != 0) { return OB_IO_ERROR; }
  if (!S_ISREG(st.st_mode)) { return file_error("file SQL: input must be a regular file"); }
  identity.device = st.st_dev; identity.inode = st.st_ino; identity.size = st.st_size;
#ifdef __APPLE__
  identity.modified = st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
  identity.changed = st.st_ctimespec.tv_sec * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
  identity.modified = st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  identity.changed = st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
#endif
  return OB_SUCCESS;
#endif
}
int FileScanReader::open_file(const std::string &path, const ObString &root)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  ret = OB_NOT_SUPPORTED;
#else
  std::string full_path;
  if (OB_FAIL(canonical(path, root, full_path))) { }
  else {
    int fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { ret = OB_IO_ERROR; }
    else {
      char descriptor_path[PATH_MAX];
#ifdef __APPLE__
      bool resolved = fcntl(fd, F_GETPATH, descriptor_path) == 0;
#else
      std::string link = "/proc/self/fd/" + std::to_string(fd);
      ssize_t n = readlink(link.c_str(), descriptor_path, sizeof(descriptor_path)-1);
      bool resolved = n >= 0;
      if (resolved) { descriptor_path[n] = 0; }
#endif
      std::string verified;
      if (!resolved || OB_FAIL(canonical(descriptor_path, root, verified)) || verified != full_path) {
        ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      } else if ((file_ = fdopen(fd, "rb")) == nullptr) { ret = OB_IO_ERROR; }
      else { fd = -1; ret = read_identity(opened_identity_); }
      if (fd >= 0) { ::close(fd); }
    }
  }
#endif
  return ret;
}
int FileScanReader::check_unchanged() const
{
  if (definition_.format == FileScanDefinition::GLOB) { return OB_SUCCESS; }
  FileIdentity now;
  int ret = read_identity(now);
  if (OB_SUCC(ret) && !(now == opened_identity_)) { ret = file_error("file SQL: input changed during scan"); }
  return ret;
}

int FileScanReader::csv_record(std::vector<Cell> &cells)
{
  cells.clear();
  Cell field;
  bool quoted = false, in_quote = false, after_quote = false, any = false;
  int64_t bytes = 0;
  auto finish = [&]() {
    field.is_null = !quoted && field.text == definition_.null_text;
    cells.push_back(std::move(field)); field = Cell(); quoted = false; after_quote = false;
  };
  while (true) {
    int c = fgetc(file_);
    if (c == EOF) {
      if (ferror(file_)) { return OB_IO_ERROR; }
      if (in_quote) { return file_error("file SQL: unterminated CSV quoted field"); }
      if (!any) { return OB_ITER_END; }
      finish(); break;
    }
    if (++bytes > MAX_RECORD_BYTES) { return file_error("file SQL: record exceeds 16 MiB limit"); }
    if (c == 0) { return file_error("file SQL: NUL byte in CSV input"); }
    any = true;
    if (in_quote) {
      if (c == definition_.quote) {
        int next = fgetc(file_);
        if (next == definition_.quote && definition_.escape == definition_.quote) {
          if (++bytes > MAX_RECORD_BYTES) { return file_error("file SQL: record exceeds 16 MiB limit"); }
          field.text.push_back(c);
        }
        else { if (next != EOF) { ungetc(next, file_); } in_quote = false; after_quote = true; }
      } else if (definition_.escape != 0 && definition_.escape != definition_.quote && c == definition_.escape) {
        int next = fgetc(file_);
        if (next == EOF) { return file_error("file SQL: incomplete CSV escape"); }
        if (next == 0 || ++bytes > MAX_RECORD_BYTES) { return file_error("file SQL: invalid CSV escape or oversized record"); }
        field.text.push_back(next);
      } else { field.text.push_back(c); }
    } else if (c == definition_.delimiter) {
      finish();
      if (cells.size() >= MAX_COLUMNS) { return file_error("file SQL: too many columns"); }
    } else if (c == '\n' || c == '\r') {
      if (c == '\r') { int next = fgetc(file_); if (next != '\n' && next != EOF) { ungetc(next, file_); } }
      if (cells.empty() && field.text.empty() && !quoted) { any = false; bytes = 0; continue; }
      finish(); break;
    } else if (after_quote) {
      return file_error("file SQL: characters after closing CSV quote");
    } else if (definition_.quote != 0 && c == definition_.quote) {
      if (!field.text.empty()) { return file_error("file SQL: quote inside unquoted CSV field"); }
      quoted = true; in_quote = true;
    } else { field.text.push_back(c); }
  }
  ++record_number_;
  for (const Cell &cell : cells) {
    int ret = valid_utf8(cell.text);
    if (OB_FAIL(ret)) { return ret; }
  }
  return OB_SUCCESS;
}

int FileScanReader::json_record(std::vector<Cell> &cells, std::vector<std::string> &names)
{
  int ret = OB_SUCCESS;
  cells.clear(); names.clear();
  std::string line;
  while (true) {
    int c = fgetc(file_);
    if (c == EOF) {
      if (ferror(file_)) { return OB_IO_ERROR; }
      if (line.find_first_not_of(" \r\t") == std::string::npos) { return OB_ITER_END; }
      break;
    }
    if (c == '\n') {
      if (line.find_first_not_of(" \r\t") == std::string::npos) { line.clear(); continue; }
      break;
    }
    if (line.size() >= MAX_RECORD_BYTES) { return file_error("file SQL: JSONL record exceeds 16 MiB limit"); }
    line.push_back(c);
  }
  ++record_number_;
  if (record_number_ == 1 && line.compare(0,3,"\xEF\xBB\xBF") == 0) { line.erase(0,3); }
  ObArenaAllocator allocator("FileJsonRow");
  ObIJsonBase *base = nullptr;
  if (OB_FAIL(valid_utf8(line))) { }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, view(line), ObJsonInType::JSON_TREE,
                                                  ObJsonInType::JSON_TREE, base))) { }
  else if (base->json_type() != ObJsonNodeType::J_OBJECT || base->element_count() > MAX_COLUMNS) {
    ret = file_error("file SQL: JSONL rows must be flat objects with at most 1024 fields");
  } else {
    JsonObjectIterator iter = base->object_iterator();
    while (OB_SUCC(ret) && !iter.end()) {
      ObJsonObjPair pair;
      if (OB_FAIL(iter.get_elem(pair))) { }
      else {
        Cell cell;
        auto type = pair.second->json_type();
        if (type == ObJsonNodeType::J_ARRAY || type == ObJsonNodeType::J_OBJECT) {
          ret = file_error("file SQL: nested JSON values are not supported");
        } else if (type == ObJsonNodeType::J_NULL) { cell.is_null = true; }
        else if (type == ObJsonNodeType::J_STRING) { cell.text = copy_string(pair.second); cell.string_value = true; }
        else {
          ObJsonBuffer buffer(&allocator);
          if (OB_SUCC(ret = pair.second->print(buffer, true))) { cell.text.assign(buffer.ptr(), buffer.length()); }
        }
        if (OB_SUCC(ret)) { names.emplace_back(pair.first.ptr(), pair.first.length()); cells.push_back(std::move(cell)); }
      }
      iter.next();
    }
  }
  return ret;
}
int FileScanReader::record(std::vector<Cell> &cells, std::vector<std::string> &names)
{
  int64_t number = record_number_ + 1;
  if ((record_number_ & 255) == 0) {
    int status = THIS_WORKER.check_status();
    if (status != OB_SUCCESS) { return status; }
    if (THIS_WORKER.is_timeout()) { return OB_TIMEOUT; }
  }
  int ret = check_unchanged();
  if (OB_SUCC(ret)) {
    ret = definition_.format == FileScanDefinition::CSV ? csv_record(cells) : json_record(cells, names);
  }
  if (ret == OB_ITER_END) {
    int check = check_unchanged();
    if (check != OB_SUCCESS) { ret = check; }
  }
  if (ret != OB_SUCCESS && ret != OB_ITER_END) {
    record_error(ret,number);
    LOG_WARN("file SQL record read failed", K(ret), "record", number);
  }
  return ret;
}
FileValueType FileScanReader::classify(const Cell &cell, int16_t &scale)
{
  scale = 0;
  if (cell.is_null) { return FileValueType::UNKNOWN; }
  int64_t n = 0; double d = 0;
  if (calendar(cell.text, scale)) { return cell.text.size() == 10 ? FileValueType::DATE : FileValueType::DATETIME; }
  if (cell.string_value) { return FileValueType::VARCHAR; }
  std::string lower = lower_ascii(cell.text);
  if (lower == "true" || lower == "false") { return FileValueType::BOOLEAN; }
  if (integer(cell.text, n)) { return FileValueType::BIGINT; }
  if (floating(cell.text, d)) { return FileValueType::DOUBLE; }
  return FileValueType::VARCHAR;
}
FileValueType FileScanReader::widen(FileValueType a, FileValueType b)
{
  if (a == b || b == FileValueType::UNKNOWN) { return a; }
  if (a == FileValueType::UNKNOWN) { return b; }
  if (a >= FileValueType::BOOLEAN && a <= FileValueType::DOUBLE
      && b >= FileValueType::BOOLEAN && b <= FileValueType::DOUBLE) { return std::max(a,b); }
  if ((a == FileValueType::DATE && b == FileValueType::DATETIME)
      || (b == FileValueType::DATE && a == FileValueType::DATETIME)) { return FileValueType::DATETIME; }
  return FileValueType::VARCHAR;
}
int FileScanReader::skip_header()
{
  if (definition_.format != FileScanDefinition::CSV || !definition_.header) { return OB_SUCCESS; }
  std::vector<Cell> cells;
  return csv_record(cells);
}
int FileScanReader::discover(const std::string &pattern, const ObString &root)
{
#ifdef _WIN32
  return OB_NOT_SUPPORTED;
#else
  size_t slash = pattern.find_last_of('/');
  std::string directory = slash == std::string::npos ? "." : pattern.substr(0, slash + 1);
  std::string base = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
  std::string checked;
  int ret = canonical(directory, root, checked);
  if (OB_FAIL(ret)) { return ret; }
  if (base.empty() || base.find('\0') != std::string::npos) { return OB_INVALID_ARGUMENT; }
  DIR *entries = opendir(checked.c_str());
  if (entries == nullptr) { return OB_IO_ERROR; }
  size_t path_bytes = 0, visited = 0;
  while (OB_SUCC(ret)) {
    if ((visited++ & 255) == 0) {
      if (OB_FAIL(THIS_WORKER.check_status())) { break; }
      if (THIS_WORKER.is_timeout()) { ret = OB_TIMEOUT; break; }
    }
    errno = 0;
    dirent *entry = readdir(entries);
    if (entry == nullptr) { if (errno != 0) { ret = OB_IO_ERROR; } break; }
    if (fnmatch(base.c_str(),entry->d_name,FNM_PERIOD) != 0) { continue; }
    std::string path;
    struct stat st;
    if (OB_FAIL(canonical(checked + "/" + entry->d_name, root, path))) { }
    else if (stat(path.c_str(), &st) != 0) { ret = OB_IO_ERROR; }
    else if (S_ISREG(st.st_mode)) {
      if (files_.size() >= 100000 || path.size() > MAX_RECORD_BYTES-path_bytes) { ret = OB_SIZE_OVERFLOW; }
      else { path_bytes += path.size(); files_.push_back(std::move(path)); }
    }
  }
  closedir(entries);
  std::sort(files_.begin(), files_.end());
  return ret;
#endif
}
int FileScanReader::infer(FileScanDefinition &definition, const ObString &root)
{
  close(); definition_ = definition;
  int ret = OB_SUCCESS;
  definition.columns.clear(); definition.row_count = 0;
  if (definition.format == FileScanDefinition::GLOB) {
    if (OB_SUCC(ret = discover(definition.path, root))) {
      definition.columns.push_back({"file", FileValueType::VARCHAR, false, 0});
      definition.row_count = files_.size();
    }
  } else if (definition.format == FileScanDefinition::PARQUET) {
    if (OB_SUCC(ret = open_file(definition.path, root))) {
      definition.identity = opened_identity_;
      ParquetFileReader parquet;
      ret = parquet.open(file_, definition);
      if (OB_SUCC(ret)) { ret = check_unchanged(); }
    }
  } else if (OB_FAIL(open_file(definition.path, root))) {
  } else {
    definition.identity = opened_identity_;
    std::vector<Cell> cells;
    std::vector<std::string> names;
    std::map<std::string, size_t> lookup;
    bool first = true;
    while (OB_SUCC(ret) && OB_SUCC(ret = record(cells, names))) {
      if (definition.format == FileScanDefinition::CSV && first) {
        for (size_t i = 0; i < cells.size(); ++i) {
          std::string base = definition.header && !cells[i].text.empty() ? cells[i].text : "column" + std::to_string(i);
          if (i == 0 && base.compare(0,3,"\xEF\xBB\xBF") == 0) { base.erase(0,3); }
          if (base.size() > 256) { ret = file_error("file SQL: column name exceeds 256 bytes"); break; }
          std::string name = base; int suffix = 2;
          while (lookup.count(lower_ascii(name))) { name = base + "__" + std::to_string(suffix++); }
          if (name.size() > 256) { ret = file_error("file SQL: column name exceeds 256 bytes"); break; }
          lookup[lower_ascii(name)] = i;
          definition.columns.push_back({name});
        }
        first = false;
        if (definition.header) { continue; }
      }
      if (definition.format == FileScanDefinition::CSV && cells.size() != definition.columns.size()) {
        ret = file_error("file SQL: CSV record has a different number of fields");
      } else {
        std::vector<bool> present(definition.columns.size(), false);
        for (size_t i = 0; OB_SUCC(ret) && i < cells.size(); ++i) {
          size_t column_index = i;
          if (definition.format == FileScanDefinition::JSONL) {
            if (names[i].empty() || names[i].size() > 256) { ret = file_error("file SQL: invalid JSON field name"); break; }
            auto found = lookup.find(lower_ascii(names[i]));
            if (found == lookup.end()) {
              if (definition.columns.size() >= MAX_COLUMNS) { ret = OB_SIZE_OVERFLOW; break; }
              column_index = definition.columns.size(); lookup[lower_ascii(names[i])] = column_index;
              definition.columns.push_back({names[i], FileValueType::UNKNOWN, definition.row_count != 0, 0});
              present.push_back(false);
            } else { column_index = found->second; }
            if (present[column_index] || definition.columns[column_index].name != names[i]) {
              ret = file_error("file SQL: ambiguous JSON field names"); break;
            }
          }
          FileColumn &column = definition.columns[column_index];
          int16_t scale = 0;
          column.type = widen(column.type, classify(cells[i], scale));
          column.scale = std::max(column.scale, scale);
          column.nullable |= cells[i].is_null;
          present[column_index] = true;
        }
        for (size_t i = 0; i < present.size(); ++i) { definition.columns[i].nullable |= !present[i]; }
        ++definition.row_count;
      }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    else if (OB_FAIL(ret)) { record_error(ret,record_number_); }
    if (OB_SUCC(ret) && definition.columns.empty()) { ret = file_error("file SQL: cannot infer columns from empty input"); }
    for (FileColumn &column : definition.columns) {
      if (column.type == FileValueType::UNKNOWN) { column.nullable = true; }
    }
  }
  close();
  return ret;
}
int FileScanReader::open(const FileScanDefinition &definition, const ObString &root,
                         const ObIArray<int64_t> *projection)
{
  close(); definition_ = definition;
  int ret = OB_SUCCESS;
  if (definition.format == FileScanDefinition::GLOB) { ret = discover(definition.path, root); }
  else if (OB_FAIL(open_file(definition.path, root))) { }
  else if (!(opened_identity_ == definition.identity)) { ret = file_error("file SQL: input changed since schema inference"); }
  else if (definition.format == FileScanDefinition::PARQUET) {
    parquet_ = new (std::nothrow) ParquetFileReader;
    if (parquet_ == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = parquet_->open(file_, definition_, projection); }
  }
  else { ret = skip_header(); }
  if (OB_FAIL(ret)) { close(); }
  return ret;
}
int FileScanReader::convert(const Cell &cell, const FileColumn &column, ObIAllocator &allocator, ObObj &value)
{
  int ret = OB_SUCCESS;
  if (cell.is_null) { value.set_null(); }
  else if (column.type == FileValueType::VARCHAR) {
    ObString owned;
    if (OB_SUCC(ret = ob_write_string(allocator, view(cell.text), owned))) {
      value.set_varchar(owned); value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
  } else if (column.type == FileValueType::BOOLEAN) { value.set_tinyint(lower_ascii(cell.text) == "true"); }
  else if (column.type == FileValueType::BIGINT || column.type == FileValueType::DOUBLE) {
    int64_t n = 0; double d = 0;
    std::string lower = lower_ascii(cell.text);
    if (lower == "true" || lower == "false") { n = lower == "true"; d = n; }
    else if (integer(cell.text, n)) { d = n; }
    else if (column.type != FileValueType::DOUBLE || !floating(cell.text,d)) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) { if (column.type == FileValueType::BIGINT) { value.set_int(n); } else { value.set_double(d); } }
  } else if (column.type == FileValueType::DATE) {
    int32_t days = 0;
    if (OB_SUCC(ret = ObTimeConverter::str_to_date(view(cell.text), days))) { value.set_date(days); }
  } else if (column.type == FileValueType::DATETIME) {
    int64_t date = 0;
    if (OB_SUCC(ret = ObTimeConverter::str_to_datetime(view(cell.text), ObTimeConvertCtx(nullptr,false), date))) { value.set_datetime(date); }
  }
  return ret;
}
int FileScanReader::next(ObIAllocator &allocator, ObIArray<ObObj> &values)
{
  values.reuse();
  int ret = OB_SUCCESS;
  if (definition_.format == FileScanDefinition::PARQUET) {
    if (OB_FAIL(check_unchanged())) { }
    else if (parquet_ == nullptr) { ret = OB_NOT_INIT; }
    else { ret = parquet_->next(allocator, values); }
  } else if (definition_.format == FileScanDefinition::GLOB) {
    if (file_index_ >= files_.size()) { return OB_ITER_END; }
    Cell cell; cell.text = files_[file_index_++];
    ObObj value;
    if (OB_SUCC(ret = convert(cell, definition_.columns[0], allocator, value))) { ret = values.push_back(value); }
  } else {
    std::vector<Cell> cells;
    std::vector<std::string> names;
    if (OB_FAIL(record(cells,names))) { return ret; }
    if (definition_.format == FileScanDefinition::CSV && cells.size() != definition_.columns.size()) {
      return record_error(file_error("file SQL: CSV record has a different number of fields"),record_number_);
    }
    for (size_t i = 0; OB_SUCC(ret) && i < definition_.columns.size(); ++i) {
      Cell cell; cell.is_null = true;
      if (definition_.format == FileScanDefinition::CSV) { cell = std::move(cells[i]); }
      else {
        for (size_t j = 0; j < names.size(); ++j) {
          if (names[j] == definition_.columns[i].name) { cell = std::move(cells[j]); break; }
        }
      }
      ObObj value;
      if (OB_SUCC(ret = convert(cell, definition_.columns[i], allocator, value))) { ret = values.push_back(value); }
    }
  }
  return ret;
}

namespace {
#ifndef _WIN32
int checked_directory(int descriptor, const ObString &root)
{
  char path[PATH_MAX];
#ifdef __APPLE__
  if (fcntl(descriptor,F_GETPATH,path) != 0) { return OB_IO_ERROR; }
#else
  std::string link = "/proc/self/fd/" + std::to_string(descriptor);
  ssize_t n = readlink(link.c_str(),path,sizeof(path)-1);
  if (n < 0 || n == sizeof(path)-1) { return OB_IO_ERROR; }
  path[n] = 0;
#endif
  std::string checked;
  return canonical(path,root,checked);
}
#endif
}
FileCsvWriter::~FileCsvWriter()
{
  if (file_ != nullptr) { fclose(file_); }
#ifndef _WIN32
  if (directory_ >= 0) {
    remove_temporary();
    ::close(directory_);
  }
#endif
}
void FileCsvWriter::remove_temporary()
{
#ifndef _WIN32
  if (directory_ >= 0 && !temporary_.empty()) {
    struct stat st;
    if (fstatat(directory_,temporary_.c_str(),&st,AT_SYMLINK_NOFOLLOW) == 0
        && st.st_dev == output_identity_.device && st.st_ino == output_identity_.inode) {
      unlinkat(directory_,temporary_.c_str(),0);
    }
  }
#endif
}
int FileCsvWriter::open(const FileScanDefinition &options, const ObString &root)
{
#ifndef __linux__
  return OB_NOT_SUPPORTED;
#else
  if (directory_ >= 0 || file_ != nullptr) { return OB_INIT_TWICE; }
  if (root.empty()) { return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; }
  options_ = options; root_.assign(root.ptr(),root.length());
  if (options.path.empty() || options.path.find('\0') != std::string::npos || options.path.size() >= PATH_MAX) {
    return OB_INVALID_ARGUMENT;
  }
  for (char c : options.null_text) {
    if (c == 0 || c == '\n' || c == '\r' || c == options.delimiter || (options.quote != 0 && c == options.quote)) {
      return file_error("file SQL: NULL text cannot contain CSV structural characters");
    }
  }
  size_t slash = options.path.find_last_of('/');
  std::string parent = slash == std::string::npos ? "." : options.path.substr(0,slash+1);
  name_ = slash == std::string::npos ? options.path : options.path.substr(slash+1);
  if (name_.empty() || name_ == "." || name_ == "..") { return OB_INVALID_ARGUMENT; }
  std::string directory;
  int ret = canonical(parent,root,directory);
  if (OB_FAIL(ret)) { return ret; }
  directory_ = ::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if (directory_ < 0) { return OB_IO_ERROR; }
  if (OB_FAIL(checked_directory(directory_,root))) { return ret; }
  struct stat st;
  if (fstatat(directory_,name_.c_str(),&st,AT_SYMLINK_NOFOLLOW) == 0) { return OB_FILE_ALREADY_EXIST; }
  if (errno != ENOENT) { return OB_IO_ERROR; }
  static std::atomic<uint64_t> serial{0};
  for (int attempt = 0; attempt < 16 && file_ == nullptr; ++attempt) {
    std::string name = ".seekdb-copy-" + std::to_string(getpid()) + "-" + std::to_string(serial.fetch_add(1));
    int fd = openat(directory_,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd < 0) { if (errno == EEXIST) { continue; } return OB_IO_ERROR; }
    temporary_ = name;
    if (fstat(fd,&st) != 0) { ::close(fd); unlinkat(directory_,name.c_str(),0); temporary_.clear(); return OB_IO_ERROR; }
    output_identity_.device = st.st_dev; output_identity_.inode = st.st_ino;
    file_ = fdopen(fd,"wb");
    if (file_ == nullptr) { ::close(fd); return OB_IO_ERROR; }
  }
  if (file_ == nullptr) { return OB_IO_ERROR; }
  if (options.header) {
    std::string row;
    for (size_t i = 0; OB_SUCC(ret) && i < options.columns.size(); ++i) {
      if (i != 0) { row.push_back(options.delimiter); }
      ret = field(options.columns[i].name,false,row);
    }
    row.push_back('\n');
    if (OB_SUCC(ret)) { ret = write(row); }
  }
  return ret;
#endif
}
int FileCsvWriter::field(const std::string &text, bool is_null, std::string &row)
{
  if (text.size() > FileScanReader::MAX_RECORD_BYTES || row.size() > FileScanReader::MAX_RECORD_BYTES) { return OB_SIZE_OVERFLOW; }
  if (is_null) { row += options_.null_text; }
  else {
    if (text.find('\0') != std::string::npos) { return OB_INVALID_DATA; }
    bool quote = text.empty() || text == options_.null_text;
    for (char c : text) { quote |= c == options_.delimiter || c == options_.quote || c == '\n' || c == '\r'; }
    if (quote && options_.quote == 0) { return file_error("file SQL: quoted output is required for this CSV value"); }
    if (quote) { row.push_back(options_.quote); }
    for (char c : text) {
      if (quote && (c == options_.quote || (options_.escape != options_.quote && c == options_.escape))) {
        if (options_.escape == 0) { return file_error("file SQL: CSV escape is required for this value"); }
        row.push_back(options_.escape);
      }
      row.push_back(c);
      if (row.size() > FileScanReader::MAX_RECORD_BYTES) { return OB_SIZE_OVERFLOW; }
    }
    if (quote) { row.push_back(options_.quote); }
  }
  return row.size() > FileScanReader::MAX_RECORD_BYTES ? OB_SIZE_OVERFLOW : OB_SUCCESS;
}
int FileCsvWriter::write(const std::string &row)
{
  if (file_ == nullptr) { return OB_NOT_INIT; }
  return fwrite(row.data(),1,row.size(),file_) == row.size() ? OB_SUCCESS : OB_IO_ERROR;
}
int FileCsvWriter::write_row(const ObIArray<ObObj> &values)
{
  if (values.count() != options_.columns.size()) { return OB_ERR_UNEXPECTED; }
  int ret = OB_SUCCESS;
  std::string row;
  for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
    if (i != 0) { row.push_back(options_.delimiter); }
    const ObObj &value = values.at(i);
    std::string text;
    if (value.is_string_type()) {
      text.assign(value.get_string_ptr(),value.get_string_len());
    } else if (!value.is_null()) {
      char buffer[2048]; int64_t size = 0;
      if (OB_SUCC(ret = value.print_plain_str_literal(buffer,sizeof(buffer),size))) { text.assign(buffer,size); }
    }
    if (OB_SUCC(ret)) { ret = field(text,value.is_null(),row); }
  }
  row.push_back('\n');
  return OB_SUCC(ret) ? write(row) : ret;
}
int FileCsvWriter::finish()
{
#ifndef __linux__
  return OB_NOT_SUPPORTED;
#else
  if (file_ == nullptr || directory_ < 0 || temporary_.empty()) { return OB_NOT_INIT; }
  int ret = checked_directory(directory_,view(root_));
  if (OB_FAIL(ret)) { return ret; }
  if (fflush(file_) != 0 || fsync(fileno(file_)) != 0) { return OB_IO_ERROR; }
  // Publish the opened inode, so replacing the temporary pathname cannot
  // substitute a symlink or different file. The destination is never replaced.
  std::string descriptor = "/proc/self/fd/" + std::to_string(fileno(file_));
  if (linkat(AT_FDCWD,descriptor.c_str(),directory_,name_.c_str(),AT_SYMLINK_FOLLOW) != 0) {
    return errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
  }
  int closed = fclose(file_); file_ = nullptr;
  remove_temporary();
  temporary_.clear();
  return closed == 0 ? OB_SUCCESS : OB_IO_ERROR;
#endif
}
} }

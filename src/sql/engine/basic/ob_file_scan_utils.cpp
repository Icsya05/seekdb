#include "sql/engine/basic/ob_file_scan_utils.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>
#include <set>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#ifdef __linux__
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {
constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_METADATA = 1024 * 1024;
constexpr size_t MAX_BINDING = 2 * 1024 * 1024;
constexpr size_t MAX_PATH = 4096;

int fail(int status, const std::string &message, std::string &error)
{
  error = message;
  return status;
}

bool utf8(const std::string &text)
{
  const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data());
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = p[i++];
    if (c < 0x80) { continue; }
    unsigned int n = 0, value = 0, minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; value = c & 0x1f; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; value = c & 0x0f; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; value = c & 7; minimum = 0x10000; }
    else { return false; }
    if (i + n > text.size()) { return false; }
    for (unsigned int j = 0; j < n; ++j) {
      if ((p[i] & 0xc0) != 0x80) { return false; }
      value = (value << 6) | (p[i++] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { return false; }
  }
  return true;
}

// Reject excessive nesting before RapidJSON's recursive DOM parser sees it.
bool bounded_json_depth(const std::string &text, const unsigned int maximum)
{
  unsigned int depth = 0;
  bool string = false, escaped = false;
  for (const char c : text) {
    if (c == 0) { return false; }
    if (string) {
      if (escaped) { escaped = false; }
      else if (c == '\\') { escaped = true; }
      else if (c == '"') { string = false; }
    } else if (c == '"') { string = true; }
    else if (c == '{' || c == '[') { if (++depth > maximum) { return false; } }
    else if (c == '}' || c == ']') { if (depth == 0) { return false; } --depth; }
  }
  return true;
}

std::string lower_ascii(std::string text)
{
  for (char &c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return text;
}

int validate_options(const Options &o, std::string &error)
{
  if (o.sample_size != -1) { return fail(INVALID, "FileSQL requires full schema inference: sample_size=-1", error); }
  const auto valid = [](char c) { return c != 0 && c != '\r' && c != '\n' && static_cast<unsigned char>(c) < 0x80; };
  if (!valid(o.delimiter) || !valid(o.quote) || !valid(o.escape) || o.delimiter == o.quote
      || o.nullstr.size() > 1024 || !utf8(o.nullstr)) {
    return fail(INVALID, "invalid CSV delimiter, quote, escape or NULL string", error);
  }
  return OK;
}

bool same(const Fingerprint &a, const Fingerprint &b)
{
  return a.device == b.device && a.inode == b.inode && a.size == b.size
      && a.mtime_ns == b.mtime_ns && a.ctime_ns == b.ctime_ns;
}

#ifdef __linux__
int64_t timestamp_token(const timespec &time)
{
  // These are comparison tokens, not SQL timestamps. Unsigned arithmetic
  // preserves nanosecond changes even for filesystem dates beyond year 2262.
  const uint64_t bits = static_cast<uint64_t>(time.tv_sec) * uint64_t(1000000000)
                      + static_cast<uint64_t>(time.tv_nsec);
  int64_t token;
  std::memcpy(&token, &bits, sizeof(token));
  return token;
}
Fingerprint fingerprint(const struct stat &st)
{
  Fingerprint f;
  f.device = st.st_dev; f.inode = st.st_ino; f.size = st.st_size;
  f.mtime_ns = timestamp_token(st.st_mtim);
  f.ctime_ns = timestamp_token(st.st_ctim);
  return f;
}

int path_parts(const std::string &path, std::vector<std::string> &parts, std::string &error)
{
  if (path.empty() || path.size() > MAX_PATH || path.find('\0') != std::string::npos || !utf8(path)) {
    return fail(DENIED, "empty, oversized or invalid file path / secure_file_priv", error);
  }
  parts.clear();
  size_t start = 0;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) { end = path.size(); }
    std::string part = path.substr(start, end - start);
    if (part == "..") { return fail(DENIED, "parent traversal is forbidden", error); }
    if (!part.empty() && part != ".") { parts.push_back(part); }
    start = end + 1;
  }
  return OK;
}

int open_error(const std::string &where, std::string &error)
{
  const int e = errno;
  return fail(e == ELOOP || e == ENOTDIR || e == EACCES || e == EPERM ? DENIED : IO_ERROR,
              where + ": " + std::strerror(e), error);
}

// Walk only directory descriptors. No pathname validation followed by an unsafe open.
int secure_open(const std::string &path, const std::string &secure_dir,
                int &fd, std::string &absolute, Fingerprint &fp, std::string &error)
{
  fd = -1;
  std::vector<std::string> base, file;
  int ret = path_parts(secure_dir, base, error);
  if (ret != OK) { return ret; }
  if (secure_dir[0] != '/') { return fail(DENIED, "secure_file_priv must be an absolute directory", error); }
  if ((ret = path_parts(path, file, error)) != OK) { return ret; }
  if (path[0] == '/') {
    if (file.size() <= base.size() || !std::equal(base.begin(), base.end(), file.begin())) {
      return fail(DENIED, "file is outside secure_file_priv", error);
    }
    file.erase(file.begin(), file.begin() + base.size());
  }
  if (file.empty()) { return fail(DENIED, "file path names a directory", error); }
  absolute.clear();
  for (const std::string &part : base) { absolute += "/" + part; }
  for (const std::string &part : file) { absolute += "/" + part; }
  if (absolute.size() > MAX_PATH) { return fail(DENIED, "file path is too long", error); }
  int dirfd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd < 0) { return open_error("open root directory", error); }
  for (const std::string &part : base) {
    int child = ::openat(dirfd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) { ret = open_error("open secure directory", error); ::close(dirfd); return ret; }
    ::close(dirfd); dirfd = child;
  }
  for (size_t i = 0; i + 1 < file.size(); ++i) {
    int child = ::openat(dirfd, file[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) { ret = open_error("open file directory", error); ::close(dirfd); return ret; }
    ::close(dirfd); dirfd = child;
  }
  // NONBLOCK prevents a hostile FIFO from blocking before fstat can reject it.
  fd = ::openat(dirfd, file.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) { ret = open_error("open file", error); }
  ::close(dirfd);
  if (ret != OK) { return ret; }
  struct stat st;
  if (::fstat(fd, &st) != 0) { ret = open_error("stat file", error); }
  else if (!S_ISREG(st.st_mode)) { ret = fail(DENIED, "FileSQL reads regular files only", error); }
  else { fp = fingerprint(st); }
  if (ret != OK) { ::close(fd); fd = -1; }
  return ret;
}
#else
int secure_open(const std::string &, const std::string &, int &fd, std::string &,
                Fingerprint &, std::string &error)
{
  fd = -1;
  return fail(UNSUPPORTED, "race-safe FileSQL opens currently require Linux", error);
}
#endif

class Reader {
public:
  Reader() : fd_(-1), pos_(0), end_(0), record_(0), bytes_read_(0), check_status_(nullptr) {}
  ~Reader() { close(); }
  void close()
  {
#ifdef __linux__
    if (fd_ >= 0) { ::close(fd_); }
#endif
    fd_ = -1; pos_ = end_ = 0; record_ = 0; bytes_read_ = 0;
  }
  int open(const std::string &path, const std::string &secure_dir, const Fingerprint *expected, std::string &error)
  {
    close(); dir_ = secure_dir;
    int ret = secure_open(path, secure_dir, fd_, path_, fp_, error);
    if (ret == OK && expected != nullptr && !same(fp_, *expected)) {
      ret = fail(CHANGED, "file changed since schema inference", error);
    }
    if (ret != OK) { close(); }
    return ret;
  }
  int check(std::string &error) const
  {
    const int status = check_status_ != nullptr && *check_status_ ? (*check_status_)() : OK;
    if (status != OK) { return fail(status, "FileSQL operation was interrupted", error); }
#ifdef __linux__
    if (fd_ < 0) { return fail(INVALID, "scanner is not open", error); }
    struct stat st;
    if (::fstat(fd_, &st) != 0) { return open_error("stat open file", error); }
    if (!S_ISREG(st.st_mode) || !same(fp_, fingerprint(st))) { return fail(CHANGED, "open file contents changed", error); }
    int fd = -1; std::string absolute; Fingerprint current;
    int ret = secure_open(path_, dir_, fd, absolute, current, error);
    if (fd >= 0) { ::close(fd); }
    if (ret != OK) { return ret == DENIED ? ret : fail(CHANGED, "bound file path disappeared", error); }
    if (!same(fp_, current)) { return fail(CHANGED, "bound file path was replaced", error); }
    return OK;
#else
    return fail(UNSUPPORTED, "FileSQL currently requires Linux", error);
#endif
  }
  int rescan(std::string &error)
  {
    int ret = check(error);
#ifdef __linux__
    if (ret == OK && ::lseek(fd_, 0, SEEK_SET) < 0) { ret = open_error("rewind file", error); }
#endif
    if (ret == OK) { pos_ = end_ = 0; record_ = 0; bytes_read_ = 0; }
    return ret;
  }
  int get(char &c, std::string &error)
  {
    int ret = peek(c, error);
    if (ret == OK) { ++pos_; }
    return ret;
  }
  int peek(char &c, std::string &error)
  {
    if (pos_ == end_) {
      int ret = check(error);
      if (ret != OK) { return ret; }
#ifdef __linux__
      ssize_t count;
      do { count = ::read(fd_, buffer_, sizeof(buffer_)); } while (count < 0 && errno == EINTR);
      if (count < 0) { return open_error("read file", error); }
      if (count > fp_.size - bytes_read_ || (count == 0 && bytes_read_ != fp_.size)) {
        return fail(CHANGED, "actual file byte count differs from the bound fingerprint", error);
      }
      bytes_read_ += count;
      pos_ = 0; end_ = static_cast<size_t>(count);
      if (count == 0) { return END; }
#else
      return fail(UNSUPPORTED, "FileSQL currently requires Linux", error);
#endif
    }
    c = buffer_[pos_]; return OK;
  }
  int begin_record(std::string &error)
  {
    const int status = check_status_ != nullptr && *check_status_ ? (*check_status_)() : OK;
    if (status != OK) { return fail(status, "FileSQL operation was interrupted", error); }
    int ret = OK;
    if (record_++ == 0) {
      char c;
      ret = peek(c, error);
      if (ret == END) { return END; }
      if (ret != OK) { return ret; }
      if (static_cast<unsigned char>(c) == 0xef && end_ - pos_ >= 3
          && static_cast<unsigned char>(buffer_[pos_+1]) == 0xbb
          && static_cast<unsigned char>(buffer_[pos_+2]) == 0xbf) {
        // A UTF-8 BOM is recognized only at the beginning of the file.
        pos_ += 3;
      }
    }
    return ret;
  }
  const std::string &path() const { return path_; }
  const Fingerprint &fp() const { return fp_; }
  uint64_t record() const { return record_; }
  void set_check_status(const std::function<int()> *check_status) { check_status_ = check_status; }
private:
  int fd_;
  char buffer_[64 * 1024];
  size_t pos_, end_;
  uint64_t record_;
  int64_t bytes_read_;
  std::string path_, dir_;
  Fingerprint fp_;
  const std::function<int()> *check_status_;
};

int csv_record(Reader &reader, const Options &options, std::vector<Cell> &row, std::string &error)
{
  row.clear();
  int ret = reader.begin_record(error);
  if (ret != OK) { return ret; }
  enum State { START, PLAIN, QUOTED, CLOSED } state = START;
  std::string text;
  bool quoted = false, any = false;
  size_t bytes = 0;
  const auto field = [&]() -> int {
    if (row.size() == MAX_COLUMNS) { return fail(LIMIT_EXCEEDED, "CSV column limit exceeded", error); }
    if (!utf8(text)) { return fail(INVALID, "invalid UTF-8 in CSV record " + std::to_string(reader.record()), error); }
    Cell cell; cell.is_null = !quoted && text == options.nullstr; cell.text.swap(text);
    row.push_back(std::move(cell)); state = START; quoted = false; return OK;
  };
  while (true) {
    char c = 0;
    ret = reader.get(c, error);
    if (ret == END) {
      if (state == QUOTED) { return fail(INVALID, "unterminated quoted CSV record " + std::to_string(reader.record()), error); }
      if (!any) { return END; }
      return field();
    }
    if (ret != OK) { return ret; }
    any = true;
    if (++bytes > MAX_RECORD) { return fail(LIMIT_EXCEEDED, "CSV record exceeds 8 MiB", error); }
    if (state == QUOTED) {
      if (c == options.quote) {
        char following = 0;
        ret = reader.peek(following, error);
        if (ret != OK && ret != END) { return ret; }
        if (options.escape == options.quote && ret == OK && following == options.quote) {
          reader.get(following, error); ++bytes; text += c;
        } else { state = CLOSED; }
      } else if (options.escape != options.quote && c == options.escape) {
        char following = 0;
        if ((ret = reader.get(following, error)) != OK) {
          return ret == END ? fail(INVALID, "unterminated CSV escape", error) : ret;
        }
        ++bytes;
        if (following != options.quote && following != options.escape) { text += c; }
        text += following;
      } else { text += c; }
    } else if (c == options.delimiter) {
      if ((ret = field()) != OK) { return ret; }
    } else if (c == '\r' || c == '\n') {
      if (c == '\r') {
        char following = 0;
        ret = reader.peek(following, error);
        if (ret != OK && ret != END) { return ret; }
        if (ret == OK && following == '\n') { reader.get(following, error); }
      }
      return field();
    } else if (c == options.quote && state == START) {
      state = QUOTED; quoted = true;
    } else if (state == CLOSED || c == options.quote) {
      return fail(INVALID, "unexpected character or quote in CSV record " + std::to_string(reader.record()), error);
    } else { state = PLAIN; text += c; }
    if (bytes > MAX_RECORD) { return fail(LIMIT_EXCEEDED, "CSV record exceeds 8 MiB", error); }
  }
}

bool digits(const std::string &s, size_t start, size_t count)
{
  if (start + count > s.size()) { return false; }
  for (size_t i = start; i < start + count; ++i) { if (s[i] < '0' || s[i] > '9') { return false; } }
  return true;
}
int decimal(const std::string &s, size_t start, size_t count)
{
  int value = 0;
  for (size_t i = start; i < start + count; ++i) { value = value * 10 + s[i] - '0'; }
  return value;
}
bool date(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !digits(s,0,4) || !digits(s,5,2) || !digits(s,8,2)) { return false; }
  int y = decimal(s,0,4), m = decimal(s,5,2), d = decimal(s,8,2);
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (y == 0 || m < 1 || m > 12 || d < 1) { return false; }
  const int leap = m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 1 : 0;
  return d <= days[m-1] + leap;
}
bool datetime(const std::string &s)
{
  if (s.size() < 19 || !date(s) || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':'
      || !digits(s,11,2) || !digits(s,14,2) || !digits(s,17,2)
      || decimal(s,11,2) > 23 || decimal(s,14,2) > 59 || decimal(s,17,2) > 59) { return false; }
  return s.size() == 19 || (s.size() >= 21 && s.size() <= 26 && s[19] == '.' && digits(s,20,s.size()-20));
}

Type number_type(const std::string &s)
{
  size_t i = 0;
  if (s.empty()) { return Type::VARCHAR; }
  if (s[i] == '-' || s[i] == '+') { ++i; }
  const size_t begin = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
  size_t count = i - begin;
  bool integer = true;
  if (i < s.size() && s[i] == '.') {
    integer = false; ++i;
    const size_t after = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
    count += i - after;
  }
  if (count == 0) { return Type::VARCHAR; }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    integer = false; ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { ++i; }
    const size_t exponent = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
    if (i == exponent) { return Type::VARCHAR; }
  }
  if (i != s.size()) { return Type::VARCHAR; }
  char *end = nullptr;
  if (integer) {
    errno = 0;
    std::strtoll(s.c_str(), &end, 10);
    if (errno != ERANGE && end == s.c_str() + s.size()) { return Type::BIGINT; }
  }
#ifdef __linux__
  // SQL sessions may change their collation; inference always uses decimal '.'.
  struct NumericLocale {
    locale_t value;
    NumericLocale() : value(newlocale(LC_NUMERIC_MASK, "C", nullptr)) {}
    ~NumericLocale() { if (value != nullptr) { freelocale(value); } }
  };
  static NumericLocale locale;
  if (locale.value == nullptr) { return Type::VARCHAR; }
  errno = 0;
  const double value = strtod_l(s.c_str(), &end, locale.value);
  return end == s.c_str() + s.size() && errno != ERANGE && std::isfinite(value) ? Type::DOUBLE : Type::VARCHAR;
#else
  return Type::VARCHAR;
#endif
}

Type text_type(const std::string &s)
{
  if (s.size() == 10 && date(s)) { return Type::DATE; }
  if (datetime(s)) { return Type::DATETIME; }
  const std::string lower = lower_ascii(s);
  if (lower == "true" || lower == "false") { return Type::BOOLEAN; }
  return number_type(s);
}
Type widen(Type a, Type b)
{
  if (a == Type::NULL_VALUE) { return b; }
  if (b == Type::NULL_VALUE || a == b) { return a; }
  if ((a == Type::BIGINT && b == Type::DOUBLE) || (a == Type::DOUBLE && b == Type::BIGINT)) { return Type::DOUBLE; }
  if ((a == Type::DATE && b == Type::DATETIME) || (a == Type::DATETIME && b == Type::DATE)) { return Type::DATETIME; }
  return Type::VARCHAR;
}
void observe(Column &column, const Cell &cell, Type type)
{
  if (cell.is_null) { column.nullable = true; }
  else {
    column.type = widen(column.type, type);
    column.max_length = std::max(column.max_length, static_cast<int64_t>(cell.text.size()));
  }
}

struct JsonRow {
  std::vector<std::string> names;
  std::vector<Cell> cells;
  std::vector<Type> types;
};

int json_record(Reader &reader, JsonRow &row, std::string &error)
{
  row.names.clear(); row.cells.clear(); row.types.clear();
  int ret = reader.begin_record(error);
  if (ret != OK) { return ret; }
  std::string line;
  while (true) {
    char c = 0;
    ret = reader.get(c, error);
    if (ret == END) { if (line.empty()) { return END; } break; }
    if (ret != OK) { return ret; }
    if (c == '\n') { break; }
    if (line.size() == MAX_RECORD) { return fail(LIMIT_EXCEEDED, "JSONL record exceeds 8 MiB", error); }
    line += c;
  }
  if (!line.empty() && line.back() == '\r') { line.pop_back(); }
  if (!bounded_json_depth(line, 1)) { return fail(INVALID, "nested JSONL values or embedded NUL are forbidden", error); }
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag>(line.data(), line.size());
  if (document.HasParseError() || !document.IsObject()) {
    return fail(INVALID, "JSONL requires a flat object at record " + std::to_string(reader.record()), error);
  }
  if (document.MemberCount() > MAX_COLUMNS) { return fail(LIMIT_EXCEEDED, "JSONL column limit exceeded", error); }
  std::set<std::string> names;
  for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
    std::string name(member->name.GetString(), member->name.GetStringLength());
    if (name.empty() || name.find('\0') != std::string::npos || !names.insert(lower_ascii(name)).second) {
      return fail(INVALID, "empty, NUL or duplicate JSONL key", error);
    }
    Cell cell; Type type = Type::NULL_VALUE;
    const auto &value = member->value;
    if (value.IsNull()) { cell.is_null = true; }
    else if (value.IsString()) {
      cell.text.assign(value.GetString(), value.GetStringLength());
      type = cell.text.size() == 10 && date(cell.text) ? Type::DATE : (datetime(cell.text) ? Type::DATETIME : Type::VARCHAR);
    } else if (value.IsBool() || value.IsNumber()) {
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      if (!value.Accept(writer)) { return fail(INVALID, "nonfinite JSONL number", error); }
      cell.text.assign(buffer.GetString(), buffer.GetSize());
      type = value.IsBool() ? Type::BOOLEAN : (value.IsInt64() ? Type::BIGINT : Type::DOUBLE);
    } else { return fail(INVALID, "nested JSONL object or array is unsupported", error); }
    row.names.push_back(std::move(name)); row.cells.push_back(std::move(cell)); row.types.push_back(type);
  }
  return OK;
}

int validate_binding(const Binding &b, std::string &error)
{
  int ret = validate_options(b.options, error);
  if (ret != OK) { return ret; }
  if ((b.format != Format::CSV && b.format != Format::JSONL) || b.path.empty() || b.path.size() > MAX_PATH
      || b.path[0] != '/' || b.path.find('\0') != std::string::npos || !utf8(b.path)
      || b.columns.empty() || b.columns.size() > MAX_COLUMNS || b.fingerprint.size < 0) {
    return fail(INVALID, "invalid FileSQL binding", error);
  }
  size_t metadata = 0;
  std::set<std::string> names;
  for (const Column &c : b.columns) {
    if (c.name.empty() || c.name.find('\0') != std::string::npos || !utf8(c.name)
        || static_cast<int>(c.type) < 0 || c.type > Type::DATETIME || c.max_length < 0
        || c.max_length > static_cast<int64_t>(MAX_RECORD) || !names.insert(lower_ascii(c.name)).second) {
      return fail(INVALID, "invalid or duplicate FileSQL column metadata", error);
    }
    metadata += c.name.size() + sizeof(Column);
    if (metadata > MAX_METADATA) { return fail(LIMIT_EXCEEDED, "FileSQL schema metadata exceeds 1 MiB", error); }
  }
  return OK;
}
} // namespace

int infer_file(const std::string &path, const std::string &secure_dir,
               Format format, const Options &options, Binding &binding, std::string &error,
               const std::function<int()> &check_status)
{
  error.clear();
  int ret = validate_options(options, error);
  if (ret != OK) { return ret; }
  if (format != Format::CSV && format != Format::JSONL) { return fail(UNSUPPORTED, "unsupported FileSQL format", error); }
  Reader reader;
  reader.set_check_status(&check_status);
  if (check_status) {
    const int status = check_status();
    if (status != OK) { return fail(status, "FileSQL inference was interrupted", error); }
  }
  if ((ret = reader.open(path, secure_dir, nullptr, error)) != OK) { return ret; }
  Binding result; result.path = reader.path(); result.fingerprint = reader.fp(); result.format = format; result.options = options;
  size_t metadata = 0;
  uint64_t count = 0;
  if (format == Format::CSV) {
    std::vector<Cell> row;
    ret = csv_record(reader, options, row, error);
    if (ret == END) { return fail(INVALID, "cannot infer columns from empty CSV", error); }
    if (ret != OK) { return ret; }
    std::set<std::string> names;
    for (size_t i = 0; i < row.size(); ++i) {
      Column column;
      const std::string original = options.header && !row[i].text.empty() ? row[i].text : "column" + std::to_string(i+1);
      column.name = original;
      size_t suffix = 2;
      while (names.count(lower_ascii(column.name)) != 0) { column.name = original + "__" + std::to_string(suffix++); }
      if (column.name.find('\0') != std::string::npos) { return fail(INVALID, "NUL in CSV header", error); }
      names.insert(lower_ascii(column.name)); metadata += column.name.size() + sizeof(Column);
      if (metadata > MAX_METADATA) { return fail(LIMIT_EXCEEDED, "CSV schema metadata exceeds 1 MiB", error); }
      result.columns.push_back(std::move(column));
    }
    if (!options.header) {
      for (size_t i = 0; i < row.size(); ++i) { observe(result.columns[i], row[i], text_type(row[i].text)); }
    }
    while ((ret = csv_record(reader, options, row, error)) == OK) {
      if (row.size() != result.columns.size()) { return fail(INVALID, "CSV field count mismatch at record " + std::to_string(reader.record()), error); }
      for (size_t i = 0; i < row.size(); ++i) { observe(result.columns[i], row[i], text_type(row[i].text)); }
    }
  } else {
    JsonRow row;
    while ((ret = json_record(reader, row, error)) == OK) {
      std::vector<bool> seen(result.columns.size(), false);
      for (size_t i = 0; i < row.names.size(); ++i) {
        size_t column = 0;
        for (; column < result.columns.size(); ++column) {
          if (result.columns[column].name == row.names[i]) { break; }
          if (lower_ascii(result.columns[column].name) == lower_ascii(row.names[i])) {
            return fail(INVALID, "JSONL keys differ only by case", error);
          }
        }
        if (column == result.columns.size()) {
          if (column == MAX_COLUMNS) { return fail(LIMIT_EXCEEDED, "JSONL schema column limit exceeded", error); }
          Column item; item.name = row.names[i]; item.nullable = count > 0;
          metadata += item.name.size() + sizeof(Column);
          if (metadata > MAX_METADATA) { return fail(LIMIT_EXCEEDED, "JSONL schema metadata exceeds 1 MiB", error); }
          result.columns.push_back(std::move(item)); seen.push_back(false);
        }
        seen[column] = true; observe(result.columns[column], row.cells[i], row.types[i]);
      }
      for (size_t i = 0; i < seen.size(); ++i) { if (!seen[i]) { result.columns[i].nullable = true; } }
      ++count;
    }
    if (ret == END && result.columns.empty()) { return fail(INVALID, "cannot infer columns from empty JSONL", error); }
  }
  if (ret != END) { return ret; }
  if ((ret = reader.check(error)) != OK) { return ret; }
  for (Column &column : result.columns) { if (column.type == Type::NULL_VALUE) { column.nullable = true; } }
  if ((ret = validate_binding(result, error)) != OK) { return ret; }
  binding = std::move(result);
  return OK;
}

class Scanner::Impl {
public:
  Binding binding;
  Reader reader;
  bool opened = false;
  int skip_header(std::string &error)
  {
    if (binding.format == Format::CSV && binding.options.header) {
      std::vector<Cell> row;
      int ret = csv_record(reader, binding.options, row, error);
      if (ret != OK) { return ret == END ? fail(CHANGED, "CSV header disappeared", error) : ret; }
      if (row.size() != binding.columns.size()) { return fail(CHANGED, "CSV header width changed", error); }
    }
    return OK;
  }
};

Scanner::Scanner() : impl_(new(std::nothrow) Impl()) {}
Scanner::~Scanner() { delete impl_; }
void Scanner::close()
{
  if (impl_ != nullptr) { impl_->reader.close(); impl_->opened = false; }
}
int Scanner::open(const Binding &binding, const std::string &secure_dir, std::string &error)
{
  error.clear(); close();
  if (impl_ == nullptr) { return fail(LIMIT_EXCEEDED, "cannot allocate scanner", error); }
  int ret = validate_binding(binding, error);
  if (ret != OK) { return ret; }
  impl_->binding = binding;
  if ((ret = impl_->reader.open(binding.path, secure_dir, &binding.fingerprint, error)) == OK
      && (ret = impl_->skip_header(error)) == OK) { impl_->opened = true; }
  if (ret != OK) { close(); }
  return ret;
}
int Scanner::next(std::vector<Cell> &row, std::string &error, const std::function<int()> &check_status)
{
  error.clear(); row.clear();
  if (impl_ == nullptr || !impl_->opened) { return fail(INVALID, "scanner is not open", error); }
  impl_->reader.set_check_status(&check_status);
  struct ResetStatus {
    Reader &reader;
    ~ResetStatus() { reader.set_check_status(nullptr); }
  } reset_status{impl_->reader};
  int ret;
  if (impl_->binding.format == Format::CSV) {
    ret = csv_record(impl_->reader, impl_->binding.options, row, error);
    if (ret == OK && row.size() != impl_->binding.columns.size()) { ret = fail(INVALID, "CSV field count mismatch", error); }
  } else {
    JsonRow object;
    ret = json_record(impl_->reader, object, error);
    if (ret == OK) {
      row.resize(impl_->binding.columns.size());
      for (Cell &cell : row) { cell.is_null = true; }
      for (size_t i = 0; i < object.names.size(); ++i) {
        size_t column = 0;
        for (; column < impl_->binding.columns.size() && object.names[i] != impl_->binding.columns[column].name; ++column) {}
        if (column == impl_->binding.columns.size()) { return fail(CHANGED, "JSONL key is absent from bound schema", error); }
        row[column] = std::move(object.cells[i]);
      }
    }
  }
  // Validate even if the current record fit entirely in a prefetched read buffer.
  if (ret == OK || ret == END) {
    const int checked = impl_->reader.check(error);
    if (checked != OK) { row.clear(); return checked; }
  }
  return ret;
}
int Scanner::rescan(std::string &error)
{
  error.clear();
  if (impl_ == nullptr || !impl_->opened) { return fail(INVALID, "scanner is not open", error); }
  int ret = impl_->reader.rescan(error);
  if (ret == OK) { ret = impl_->skip_header(error); }
  return ret;
}

int serialize_binding(const Binding &binding, std::string &json, std::string &error)
{
  error.clear();
  int ret = validate_binding(binding, error);
  if (ret != OK) { return ret; }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  const auto str = [&](const std::string &s) { writer.String(s.data(), static_cast<rapidjson::SizeType>(s.size())); };
  writer.StartObject(); writer.Key("version"); writer.Int(1);
  writer.Key("format"); writer.Int(static_cast<int>(binding.format));
  writer.Key("path"); str(binding.path);
  writer.Key("options"); writer.StartObject();
  writer.Key("header"); writer.Bool(binding.options.header);
  writer.Key("delimiter"); writer.Int(static_cast<unsigned char>(binding.options.delimiter));
  writer.Key("quote"); writer.Int(static_cast<unsigned char>(binding.options.quote));
  writer.Key("escape"); writer.Int(static_cast<unsigned char>(binding.options.escape));
  writer.Key("nullstr"); str(binding.options.nullstr);
  writer.Key("sample_size"); writer.Int64(binding.options.sample_size); writer.EndObject();
  writer.Key("fingerprint"); writer.StartArray(); writer.Uint64(binding.fingerprint.device); writer.Uint64(binding.fingerprint.inode);
  writer.Int64(binding.fingerprint.size); writer.Int64(binding.fingerprint.mtime_ns); writer.Int64(binding.fingerprint.ctime_ns); writer.EndArray();
  writer.Key("columns"); writer.StartArray();
  for (const Column &column : binding.columns) {
    writer.StartObject(); writer.Key("name"); str(column.name); writer.Key("type"); writer.Int(static_cast<int>(column.type));
    writer.Key("max_length"); writer.Int64(column.max_length); writer.Key("nullable"); writer.Bool(column.nullable); writer.EndObject();
  }
  writer.EndArray(); writer.EndObject();
  if (buffer.GetSize() > MAX_BINDING) { return fail(LIMIT_EXCEEDED, "serialized FileSQL binding exceeds 2 MiB", error); }
  json.assign(buffer.GetString(), buffer.GetSize());
  return OK;
}

int deserialize_binding(const std::string &json, Binding &binding, std::string &error)
{
  error.clear();
  if (json.size() > MAX_BINDING) { return fail(LIMIT_EXCEEDED, "FileSQL binding exceeds 2 MiB", error); }
  if (!bounded_json_depth(json, 8)) { return fail(INVALID, "invalid or excessively nested FileSQL binding", error); }
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
  const auto member = [](const rapidjson::Value &v, const char *key) -> const rapidjson::Value * {
    if (!v.IsObject()) { return nullptr; }
    auto found = v.FindMember(key); return found == v.MemberEnd() ? nullptr : &found->value;
  };
  const auto required = [&](const rapidjson::Value &v, const char *key, int kind) -> const rapidjson::Value * {
    const auto *p = member(v,key);
    if (p == nullptr) { return nullptr; }
    const bool valid = kind == 0 ? p->IsInt() : kind == 1 ? p->IsString() : kind == 2 ? p->IsBool()
                    : kind == 3 ? p->IsObject() : kind == 4 ? p->IsArray() : p->IsInt64();
    return valid ? p : nullptr;
  };
  const auto *version = required(doc,"version",0), *format = required(doc,"format",0), *path = required(doc,"path",1);
  const auto *options = required(doc,"options",3), *fp = required(doc,"fingerprint",4), *columns = required(doc,"columns",4);
  if (doc.HasParseError() || version == nullptr || version->GetInt() != 1 || format == nullptr
      || format->GetInt() < 0 || format->GetInt() > 1 || path == nullptr || options == nullptr || fp == nullptr || columns == nullptr
      || fp->Size() != 5 || !(*fp)[0].IsUint64() || !(*fp)[1].IsUint64() || !(*fp)[2].IsInt64()
      || !(*fp)[3].IsInt64() || !(*fp)[4].IsInt64() || columns->Empty() || columns->Size() > MAX_COLUMNS) {
    return fail(INVALID, "malformed FileSQL binding", error);
  }
  const auto *header = required(*options,"header",2), *delimiter = required(*options,"delimiter",0), *quote = required(*options,"quote",0);
  const auto *escape = required(*options,"escape",0), *nullstr = required(*options,"nullstr",1), *sample = required(*options,"sample_size",5);
  if (header == nullptr || delimiter == nullptr || quote == nullptr || escape == nullptr || nullstr == nullptr || sample == nullptr
      || delimiter->GetInt() < 1 || delimiter->GetInt() > 127 || quote->GetInt() < 1 || quote->GetInt() > 127
      || escape->GetInt() < 1 || escape->GetInt() > 127) { return fail(INVALID, "malformed FileSQL options", error); }
  Binding result;
  result.format = static_cast<Format>(format->GetInt()); result.path.assign(path->GetString(),path->GetStringLength());
  result.options.header = header->GetBool(); result.options.delimiter = static_cast<char>(delimiter->GetInt());
  result.options.quote = static_cast<char>(quote->GetInt()); result.options.escape = static_cast<char>(escape->GetInt());
  result.options.nullstr.assign(nullstr->GetString(),nullstr->GetStringLength()); result.options.sample_size = sample->GetInt64();
  result.fingerprint.device = (*fp)[0].GetUint64(); result.fingerprint.inode = (*fp)[1].GetUint64();
  result.fingerprint.size = (*fp)[2].GetInt64(); result.fingerprint.mtime_ns = (*fp)[3].GetInt64(); result.fingerprint.ctime_ns = (*fp)[4].GetInt64();
  for (const auto &item : columns->GetArray()) {
    const auto *name = required(item,"name",1), *type = required(item,"type",0), *length = required(item,"max_length",5), *nullable = required(item,"nullable",2);
    if (name == nullptr || type == nullptr || length == nullptr || nullable == nullptr || type->GetInt() < 0 || type->GetInt() > 6) {
      return fail(INVALID, "malformed FileSQL column", error);
    }
    Column column; column.name.assign(name->GetString(),name->GetStringLength()); column.type = static_cast<Type>(type->GetInt());
    column.max_length = length->GetInt64(); column.nullable = nullable->GetBool(); result.columns.push_back(std::move(column));
  }
  int ret = validate_binding(result,error);
  if (ret == OK) { binding = std::move(result); }
  return ret;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase

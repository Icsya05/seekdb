/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_parquet_file_reader.h"
#include "share/ob_errno.h"
#include "lib/charset/ob_charset.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
constexpr size_t PAGE_LIMIT = 16 * 1024 * 1024;
constexpr size_t MEMORY_LIMIT = 128 * 1024 * 1024;
constexpr size_t VALUE_LIMIT = 1024 * 1024;

struct Bytes {
  const uint8_t *data;
  size_t size, pos = 0;
  bool ok = true;
  explicit Bytes(const std::string &s) : data(reinterpret_cast<const uint8_t *>(s.data())), size(s.size()) {}
  Bytes(const uint8_t *p, size_t n) : data(p), size(n) {}
  bool take(size_t n) { if (!ok || pos > size || n > size-pos) { ok = false; } return ok; }
  uint64_t little(size_t n) {
    uint64_t x = 0;
    if (n > 8 || !take(n)) { ok = false; return 0; }
    for (size_t i = 0; i < n; ++i) { x |= uint64_t(data[pos++]) << (i*8); }
    return x;
  }
  uint64_t varint() {
    uint64_t x = 0;
    for (unsigned shift = 0; ok && shift <= 63; shift += 7) {
      uint64_t b = little(1);
      if (shift == 63 && b > 1) { ok = false; break; }
      x |= (b & 127) << shift;
      if (!(b & 128)) { return x; }
    }
    ok = false; return 0;
  }
  int64_t zigzag() { uint64_t x = varint(); return static_cast<int64_t>((x >> 1) ^ (0 - (x & 1))); }
  std::string text(size_t n) {
    if (!take(n)) { return {}; }
    std::string s(reinterpret_cast<const char *>(data+pos), n); pos += n; return s;
  }
};

// A bounded compact-protocol metadata tree. The SQL engine never delegates
// query execution to a third-party file/query engine.
struct Meta {
  int type = 0, id = 0;
  int64_t number = 0;
  std::string text;
  std::vector<Meta> children;
  const Meta *field(int n) const {
    for (const auto &c : children) { if (c.id == n) { return &c; } }
    return nullptr;
  }
  int64_t integer(int n, int64_t fallback = -1) const {
    const Meta *v = field(n);
    return v != nullptr && v->type >= 1 && v->type <= 6 ? v->number : fallback;
  }
};
bool parse_meta(Bytes &b, Meta &out, int type, size_t &nodes, int depth = 0) {
  if (++nodes > 65536 || depth > 16) { return false; }
  out.type = type;
  if (type == 1 || type == 2) { out.number = type == 1; }
  else if (type == 3) { out.number = static_cast<int8_t>(b.little(1)); }
  else if (type >= 4 && type <= 6) { out.number = b.zigzag(); }
  else if (type == 7) { b.little(8); }
  else if (type == 8) { size_t n = b.varint(); out.text = b.text(n); }
  else if (type == 9 || type == 10) {
    uint8_t h = b.little(1); size_t n = h >> 4; int element = h & 15;
    if (n == 15) { n = b.varint(); }
    if (n > 65536-nodes || element == 0 || element > 12) { return false; }
    for (size_t i = 0; b.ok && i < n; ++i) {
      Meta child;
      int t = element <= 2 ? b.little(1) : element;
      if (!parse_meta(b, child, t, nodes, depth+1)) { return false; }
      out.children.push_back(std::move(child));
    }
  } else if (type == 11) {
    size_t n = b.varint(); uint8_t h = n ? b.little(1) : 0;
    if (n > (65536-nodes)/2) { return false; }
    for (size_t i = 0; b.ok && i < n*2; ++i) {
      Meta child; int t = i%2 == 0 ? h >> 4 : h & 15;
      if (t <= 2) { t = b.little(1); }
      if (!parse_meta(b, child, t, nodes, depth+1)) { return false; }
      out.children.push_back(std::move(child));
    }
  } else if (type == 12) {
    int id = 0;
    while (b.ok) {
      uint8_t h = b.little(1);
      if (h == 0) { break; }
      int64_t next = h >> 4 ? id + (h >> 4) : b.zigzag();
      if (next <= 0 || next > INT16_MAX || out.field(next) != nullptr) { return false; }
      Meta child; child.id = id = next;
      if (!parse_meta(b, child, h & 15, nodes, depth+1)) { return false; }
      out.children.push_back(std::move(child));
    }
  } else { return false; }
  return b.ok;
}
int invalid(const char *message) { LOG_USER_ERROR(OB_INVALID_ARGUMENT, message); return OB_INVALID_ARGUMENT; }
int unsupported(const char *message) { LOG_USER_ERROR(OB_NOT_SUPPORTED, message); return OB_NOT_SUPPORTED; }
bool read_at(FILE *file, int64_t offset, size_t size, int64_t end, std::string &out) {
  if (offset < 0 || offset > end || size > static_cast<uint64_t>(end-offset) || size > PAGE_LIMIT) { return false; }
  out.resize(size);
#ifdef _WIN32
  if (_fseeki64(file, offset, SEEK_SET) != 0) { return false; }
#else
  if (fseeko(file, offset, SEEK_SET) != 0) { return false; }
#endif
  return fread(out.data(), 1, size, file) == size;
}
bool snappy(const std::string &input, size_t expected, std::string &output) {
  Bytes b(input);
  if (b.varint() != expected || expected > PAGE_LIMIT) { return false; }
  output.clear(); output.reserve(expected);
  while (b.ok && b.pos < b.size && output.size() < expected) {
    unsigned tag = b.little(1), kind = tag & 3;
    size_t length = 0, offset = 0;
    if (kind == 0) {
      unsigned code = tag >> 2;
      length = code < 60 ? code+1 : b.little(code-59)+1;
      if (length > expected-output.size() || !b.take(length)) { return false; }
      output.append(reinterpret_cast<const char *>(b.data+b.pos), length); b.pos += length;
    } else {
      if (kind == 1) { length = 4 + ((tag >> 2) & 7); offset = ((tag & 224) << 3) | b.little(1); }
      else { length = 1 + (tag >> 2); offset = b.little(kind == 2 ? 2 : 4); }
      if (!b.ok || offset == 0 || offset > output.size() || length > expected-output.size()) { return false; }
      for (size_t i = 0; i < length; ++i) { output.push_back(output[output.size()-offset]); }
    }
  }
  return b.ok && b.pos == b.size && output.size() == expected;
}
bool unpack(Bytes &b, unsigned width, size_t count, std::vector<uint32_t> &out) {
  if (width > 32 || count > VALUE_LIMIT) { return false; }
  out.clear(); out.reserve(count);
  while (b.ok && out.size() < count) {
    uint64_t header = b.varint();
    if (header == 0) { return false; }
    uint64_t run = header >> 1;
    if (header & 1) {
      if (run > (VALUE_LIMIT+7)/8 || !b.take(run*width)) { return false; }
      size_t start = b.pos;
      for (size_t i = 0; i < run*8 && out.size() < count; ++i) {
        uint32_t n = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          size_t position = i*width + bit;
          n |= uint32_t((b.data[start+position/8] >> (position%8)) & 1) << bit;
        }
        out.push_back(n);
      }
      b.pos += run*width;
    } else {
      uint64_t n = b.little((width+7)/8);
      if (!b.ok || (width < 32 && n >= (uint64_t(1) << width)) || run > count-out.size()) { return false; }
      out.insert(out.end(), run, n);
    }
  }
  return b.ok && out.size() == count;
}
struct Scalar { int64_t integer = 0; double floating = 0; std::string text; };
struct Schema {
  FileColumn column;
  int physical = -1, length = 0, converted = -1, timestamp_unit = 0;
};
struct Chunk { int64_t offset = 0, end = 0, rows = 0; int codec = 0; };
struct Group { int64_t rows = 0; std::vector<Chunk> columns; };
struct Column {
  size_t schema = 0;
  Chunk chunk;
  int64_t position = 0, read_rows = 0;
  std::string page;
  std::vector<Scalar> dictionary;
  std::vector<uint32_t> levels, indices;
  size_t row = 0, count = 0, value = 0, plain_pos = 0, bool_bit = 0;
  int encoding = 0;
};
bool plain(Bytes &b, const Schema &s, Scalar &value, size_t &bool_bit) {
  if (s.physical == 0) {
    if (!b.take(1)) { return false; }
    value.integer = (b.data[b.pos] >> bool_bit) & 1;
    if (++bool_bit == 8) { bool_bit = 0; ++b.pos; }
  } else if (s.physical == 1) { value.integer = static_cast<int32_t>(b.little(4)); }
  else if (s.physical == 2) { value.integer = static_cast<int64_t>(b.little(8)); }
  else if (s.physical == 4) { uint32_t raw = b.little(4); float f; memcpy(&f, &raw, 4); value.floating = f; }
  else if (s.physical == 5) { uint64_t raw = b.little(8); memcpy(&value.floating, &raw, 8); }
  else if (s.physical == 6 || s.physical == 7) {
    size_t n = s.physical == 6 ? b.little(4) : s.length;
    value.text = b.text(n);
  } else { return false; }
  return b.ok;
}
}

struct ParquetFileReader::State {
  FILE *file = nullptr;
  int64_t footer = 0, total_rows = 0, rows_read = 0, group_row = 0;
  size_t group_index = 0;
  std::vector<Schema> schema;
  std::vector<Group> groups;
  std::vector<size_t> projection;
  std::vector<Column> columns;
  int metadata(FileScanDefinition &definition);
  int load_page(Column &column);
  int next_column(Column &column, ObIAllocator &allocator, ObObj &value);
  size_t memory() const {
    size_t total = 0;
    for (const Column &c : columns) {
      total += c.page.capacity() + (c.levels.capacity()+c.indices.capacity())*sizeof(uint32_t)
               + c.dictionary.capacity()*sizeof(Scalar);
      for (const Scalar &s : c.dictionary) { total += s.text.capacity(); }
    }
    return total;
  }
};

int ParquetFileReader::State::metadata(FileScanDefinition &definition)
{
  int64_t size = definition.identity.size;
  std::string bytes;
  if (size < 12 || !read_at(file, 0, 4, size, bytes) || bytes != "PAR1"
      || !read_at(file, size-8, 8, size, bytes) || bytes.substr(4) != "PAR1") {
    return invalid("file SQL: invalid Parquet magic or footer");
  }
  Bytes tail(bytes); uint64_t length = tail.little(4);
  if (length == 0 || length > PAGE_LIMIT || length > static_cast<uint64_t>(size-12)) { return OB_SIZE_OVERFLOW; }
  footer = size-8-length;
  if (!read_at(file, footer, length, size, bytes)) { return OB_IO_ERROR; }
  Bytes input(bytes); Meta root; size_t nodes = 0;
  if (!parse_meta(input, root, 12, nodes) || input.pos != input.size) { return invalid("file SQL: malformed Parquet metadata"); }
  const Meta *fields = root.field(2), *row_groups = root.field(4);
  total_rows = root.integer(3);
  if ((root.integer(1) != 1 && root.integer(1) != 2) || root.field(8) != nullptr
      || fields == nullptr || fields->type != 9 || fields->children.size() < 2
      || fields->children.size() > FileScanReader::MAX_COLUMNS+1 || row_groups == nullptr
      || row_groups->type != 9 || total_rows < 0) { return unsupported("Parquet schema or encryption"); }
  if (fields->children[0].integer(5) != static_cast<int64_t>(fields->children.size()-1)) { return unsupported("nested Parquet schema"); }
  std::set<std::string> names;
  definition.columns.clear();
  for (size_t i = 1; i < fields->children.size(); ++i) {
    const Meta &f = fields->children[i]; Schema s;
    const Meta *name = f.field(4);
    s.physical = f.integer(1); s.length = f.integer(2); s.converted = f.integer(6);
    int repetition = f.integer(3);
    if (f.type != 12 || name == nullptr || name->type != 8 || name->text.empty() || name->text.size() > 256
        || f.integer(5,0) != 0 || repetition < 0 || repetition > 1 || s.physical < 0 || s.physical > 7
        || s.physical == 3 || (s.physical == 7 && (s.length < 1 || s.length > PAGE_LIMIT))) {
      return unsupported("nested, repeated or unsupported Parquet type");
    }
    s.column.name = name->text; s.column.nullable = repetition == 1;
    std::string lower = name->text;
    for (char &c : lower) { if (c >= 'A' && c <= 'Z') { c += 'a'-'A'; } }
    if (!names.insert(lower).second) { return invalid("file SQL: duplicate Parquet column names"); }
    s.column.type = s.physical == 0 ? FileValueType::BOOLEAN
                  : s.physical <= 2 ? FileValueType::BIGINT
                  : s.physical <= 5 ? FileValueType::DOUBLE : FileValueType::VARCHAR;
    const Meta *logical = f.field(10);
    if (s.converted == 6 || (logical != nullptr && logical->field(6) != nullptr)) {
      if (s.physical != 1) { return OB_INVALID_DATA; }
      s.column.type = FileValueType::DATE;
    } else if (s.converted == 9 || s.converted == 10 || (logical != nullptr && logical->field(8) != nullptr)) {
      if (s.physical != 2) { return OB_INVALID_DATA; }
      s.timestamp_unit = s.converted == 9 ? 1 : 2;
      if (logical != nullptr && logical->field(8) != nullptr) {
        const Meta *units = logical->field(8)->field(2);
        if (units == nullptr || units->children.size() != 1) { return OB_INVALID_DATA; }
        s.timestamp_unit = units->children[0].id;
        if (s.timestamp_unit < 1 || s.timestamp_unit > 3) { return OB_INVALID_DATA; }
      }
      s.column.type = FileValueType::DATETIME; s.column.scale = s.timestamp_unit == 1 ? 3 : 6;
    } else if (s.converted == 5 || (logical != nullptr && logical->field(5) != nullptr)) {
      return unsupported("Parquet decimal logical type");
    } else if (s.converted >= 11 && s.converted <= 14) {
      return unsupported("unsigned Parquet integer logical type");
    } else if (logical != nullptr && logical->field(10) != nullptr
               && logical->field(10)->integer(2,1) == 0) {
      return unsupported("unsigned Parquet integer logical type");
    }
    schema.push_back(s); definition.columns.push_back(s.column);
  }
  int64_t row_sum = 0;
  for (const Meta &g : row_groups->children) {
    Group group; group.rows = g.integer(3);
    const Meta *chunks = g.field(1);
    if (group.rows < 0 || group.rows > total_rows-row_sum || chunks == nullptr || chunks->type != 9
        || chunks->children.size() != schema.size()) { return OB_INVALID_DATA; }
    for (size_t i = 0; i < schema.size(); ++i) {
      const Meta &chunk = chunks->children[i]; const Meta *m = chunk.field(3);
      if (chunk.field(1) != nullptr || chunk.field(8) != nullptr || m == nullptr) { return unsupported("external or encrypted Parquet column chunk"); }
      const Meta *path = m->field(3);
      if (m->integer(1) != schema[i].physical || path == nullptr || path->type != 9
          || path->children.size() != 1 || path->children[0].text != schema[i].column.name
          || m->integer(5) != group.rows) { return OB_INVALID_DATA; }
      Chunk c; c.rows = group.rows; c.codec = m->integer(4);
      c.offset = m->integer(11, m->integer(9));
      int64_t length = m->integer(7);
      if (c.offset < 4 || c.offset > footer || length < 0 || length > footer-c.offset) { return OB_INVALID_DATA; }
      c.end = c.offset+length;
      group.columns.push_back(c);
    }
    row_sum += group.rows; groups.push_back(std::move(group));
  }
  if (row_sum != total_rows) { return OB_INVALID_DATA; }
  definition.row_count = total_rows;
  return OB_SUCCESS;
}

int ParquetFileReader::State::load_page(Column &c)
{
  const Schema &s = schema[c.schema];
  while (c.position < c.chunk.end) {
    c.page.clear(); c.levels.clear(); c.indices.clear();
    if (memory() > MEMORY_LIMIT-3*PAGE_LIMIT) { return OB_SIZE_OVERFLOW; }
    std::string header_bytes;
    size_t n = std::min<int64_t>(65536, c.chunk.end-c.position);
    if (!read_at(file,c.position,n,c.chunk.end,header_bytes)) { return OB_IO_ERROR; }
    Bytes input(header_bytes); Meta header; size_t nodes = 0;
    if (!parse_meta(input,header,12,nodes)) { return invalid("file SQL: malformed Parquet page header"); }
    c.position += input.pos;
    int kind = header.integer(1);
    int64_t expanded = header.integer(2), compressed = header.integer(3);
    if (expanded < 0 || expanded > PAGE_LIMIT || compressed < 0 || compressed > PAGE_LIMIT) { return OB_SIZE_OVERFLOW; }
    std::string packed;
    if (!read_at(file,c.position,compressed,c.chunk.end,packed)) { return OB_IO_ERROR; }
    c.position += compressed;
    const Meta *v2 = kind == 3 ? header.field(8) : nullptr;
    int64_t prefix = v2 == nullptr ? 0 : v2->integer(5)+v2->integer(6);
    if (prefix < 0 || prefix > compressed || prefix > expanded) { return OB_INVALID_DATA; }
    bool uncompressed = c.chunk.codec == 0 || (v2 != nullptr && v2->integer(7,1) == 0);
    if (uncompressed) {
      if (compressed != expanded) { return OB_INVALID_DATA; }
      c.page = std::move(packed);
    } else if (c.chunk.codec == 1) {
      std::string decoded;
      if (!snappy(packed.substr(prefix),expanded-prefix,decoded)) { return invalid("file SQL: invalid Snappy page"); }
      c.page.assign(packed.data(),prefix); c.page += decoded;
    } else { return unsupported("Parquet compression codec"); }
    if (kind == 2) {
      const Meta *dict = header.field(7);
      int64_t count = dict == nullptr ? -1 : dict->integer(1);
      if (dict == nullptr || dict->integer(2) != 0 || count < 0 || count > 100000
          || !c.dictionary.empty() || c.read_rows != 0) { return OB_INVALID_DATA; }
      Bytes values(c.page); size_t bit = 0;
      for (int64_t i = 0; i < count; ++i) {
        Scalar value;
        if (!plain(values,s,value,bit)) { return OB_INVALID_DATA; }
        c.dictionary.push_back(std::move(value));
      }
      if (memory() > MEMORY_LIMIT) { return OB_SIZE_OVERFLOW; }
      continue;
    }
    const Meta *data = kind == 0 ? header.field(5) : v2;
    if (data == nullptr || (kind != 0 && kind != 3)) { return unsupported("Parquet page type"); }
    int64_t count = data->integer(1);
    if (count <= 0 || count > VALUE_LIMIT || count > c.chunk.rows-c.read_rows) { return OB_INVALID_DATA; }
    c.count = count; c.row = 0; c.value = 0; c.bool_bit = 0;
    c.encoding = data->integer(kind == 0 ? 2 : 4);
    Bytes values(c.page);
    if (kind == 3) {
      if (data->integer(6) != 0 || data->integer(3) != count || data->integer(5) < 0) { return OB_INVALID_DATA; }
      if (s.column.nullable) {
        Bytes levels(values.data, data->integer(5));
        if (!unpack(levels,1,count,c.levels) || levels.pos != levels.size) { return OB_INVALID_DATA; }
      } else if (data->integer(5) != 0) { return OB_INVALID_DATA; }
      values.pos = prefix;
    } else if (s.column.nullable) {
      if (data->integer(3) != 3) { return unsupported("Parquet definition-level encoding"); }
      size_t bytes = values.little(4);
      if (!values.take(bytes)) { return OB_INVALID_DATA; }
      Bytes levels(values.data+values.pos,bytes);
      if (!unpack(levels,1,count,c.levels) || levels.pos != levels.size) { return OB_INVALID_DATA; }
      values.pos += bytes;
    }
    size_t present = s.column.nullable ? std::count(c.levels.begin(),c.levels.end(),1) : count;
    if (kind == 3 && data->integer(2) != count-static_cast<int64_t>(present)) { return OB_INVALID_DATA; }
    if (c.encoding == 2 || c.encoding == 8) {
      if (c.dictionary.empty() && present != 0) { return OB_INVALID_DATA; }
      unsigned width = values.little(1);
      if (!unpack(values,width,present,c.indices)) { return OB_INVALID_DATA; }
      for (auto i : c.indices) { if (i >= c.dictionary.size()) { return OB_INVALID_DATA; } }
    } else if (c.encoding != 0) { return unsupported("Parquet data encoding"); }
    c.plain_pos = values.pos;
    if (memory() > MEMORY_LIMIT) { return OB_SIZE_OVERFLOW; }
    return OB_SUCCESS;
  }
  return invalid("file SQL: Parquet column ended before row count");
}

int ParquetFileReader::State::next_column(Column &c, ObIAllocator &allocator, ObObj &out)
{
  int ret = OB_SUCCESS;
  if (c.row == c.count && OB_FAIL(load_page(c))) { return ret; }
  const Schema &s = schema[c.schema];
  bool present = !s.column.nullable || c.levels[c.row] != 0;
  ++c.row; ++c.read_rows;
  if (!present) { out.set_null(); return OB_SUCCESS; }
  Scalar value;
  if (c.encoding == 2 || c.encoding == 8) {
    if (c.value >= c.indices.size()) { return OB_INVALID_DATA; }
    value = c.dictionary[c.indices[c.value++]];
  } else {
    Bytes bytes(c.page); bytes.pos = c.plain_pos;
    if (!plain(bytes,s,value,c.bool_bit)) { return OB_INVALID_DATA; }
    c.plain_pos = bytes.pos;
  }
  if (s.column.type == FileValueType::BOOLEAN) { out.set_tinyint(value.integer); }
  else if (s.column.type == FileValueType::BIGINT) { out.set_int(value.integer); }
  else if (s.column.type == FileValueType::DOUBLE) {
    if (!std::isfinite(value.floating)) { return invalid("file SQL: non-finite Parquet number"); }
    out.set_double(value.floating);
  } else if (s.column.type == FileValueType::DATE) {
    if (value.integer < -719162 || value.integer > 2932896) { return OB_INVALID_DATA; }
    out.set_date(value.integer);
  } else if (s.column.type == FileValueType::DATETIME) {
    if (s.timestamp_unit == 1) {
      if (value.integer < INT64_MIN/1000 || value.integer > INT64_MAX/1000) { return OB_INVALID_DATA; }
      value.integer *= 1000;
    } else if (s.timestamp_unit == 3) {
      if (value.integer%1000 != 0) { return unsupported("Parquet timestamp precision beyond microseconds"); }
      value.integer /= 1000;
    }
    if (value.integer < -62135596800000000LL || value.integer > 253402300799999999LL) { return OB_INVALID_DATA; }
    out.set_datetime(value.integer);
  } else {
    int64_t length = 0; int32_t error = 0; ObString owned;
    ret = ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN,value.text.data(),value.text.size(),length,error);
    if (OB_SUCC(ret) && (error != 0 || length != value.text.size())) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) { ret = ob_write_string(allocator,ObString(value.text.size(),value.text.data()),owned); }
    if (OB_SUCC(ret)) { out.set_varchar(owned); out.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
  }
  return ret;
}

ParquetFileReader::ParquetFileReader() : state_(nullptr) {}
ParquetFileReader::~ParquetFileReader() { delete state_; }
int ParquetFileReader::open(FILE *file, FileScanDefinition &definition, const ObIArray<int64_t> *projection)
{
  delete state_; state_ = new (std::nothrow) State;
  if (state_ == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  state_->file = file;
  int ret = state_->metadata(definition);
  if (OB_SUCC(ret)) {
    if (projection == nullptr) {
      for (size_t i = 0; i < state_->schema.size(); ++i) { state_->projection.push_back(i); }
    } else {
      for (int64_t p = 0; p < projection->count(); ++p) {
        int64_t i = projection->at(p);
        if (i < 0 || i >= state_->schema.size()) { return OB_INVALID_ARGUMENT; }
        state_->projection.push_back(i);
      }
    }
  }
  return ret;
}
int ParquetFileReader::next(ObIAllocator &allocator, ObIArray<ObObj> &values)
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  State &s = *state_;
  if (s.rows_read == s.total_rows) { return OB_ITER_END; }
  while (s.group_index < s.groups.size() && s.group_row == s.groups[s.group_index].rows) {
    ++s.group_index; s.group_row = 0; s.columns.clear();
  }
  if (s.group_index >= s.groups.size()) { return OB_INVALID_DATA; }
  if (s.columns.empty()) {
    for (size_t i : s.projection) {
      Column c; c.schema = i; c.chunk = s.groups[s.group_index].columns[i]; c.position = c.chunk.offset;
      s.columns.push_back(std::move(c));
    }
  }
  values.reuse(); int ret = OB_SUCCESS;
  for (size_t i = 0; OB_SUCC(ret) && i < s.schema.size(); ++i) { ObObj null; null.set_null(); ret = values.push_back(null); }
  for (Column &c : s.columns) { if (OB_SUCC(ret)) { ret = s.next_column(c,allocator,values.at(c.schema)); } }
  if (OB_SUCC(ret)) { ++s.group_row; ++s.rows_read; }
  return ret;
}
} }

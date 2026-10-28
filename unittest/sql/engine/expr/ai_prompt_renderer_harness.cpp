// Component harness for the real renderer region of ob_ai_func_utils.cpp.
// Lightweight JSON/string doubles isolate the algorithm from the server runtime.
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr int OB_SUCCESS = 0;
static constexpr int OB_INVALID_ARGUMENT = -4002;
static constexpr int OB_ALLOCATE_MEMORY_FAILED = -4013;
static constexpr int OB_BUF_NOT_ENOUGH = -4024;
static constexpr int OB_SIZE_OVERFLOW = -4019;
#define INIT_SUCC(ret) int ret = OB_SUCCESS
#define OB_SUCC(ret) ((ret) == OB_SUCCESS)
#define OB_FAIL(expr) ((ret = (expr)) != OB_SUCCESS)
#define OB_ISNULL(expr) is_null(expr)
#define OB_NOT_NULL(expr) (!is_null(expr))
#define LOG_USER_ERROR(...) ((void)0)
#define MEMCPY std::memcpy

class ObString {
public:
  ObString() : data_(nullptr), length_(0) {}
  explicit ObString(const char *text) : data_(text), length_(std::strlen(text)) {}
  void assign_ptr(const char *text, int32_t length) { data_ = text; length_ = length; }
  int32_t length() const { return length_; }
  const char *ptr() const { return data_; }
  bool empty() const { return length_ == 0; }
private:
  const char *data_;
  int32_t length_;
};
template <class T> bool is_null(T *value) { return value == nullptr; }
static bool is_null(const ObString &value) { return value.ptr() == nullptr; }

class ObIAllocator {
public:
  ~ObIAllocator() { for (void *block : blocks_) std::free(block); }
  void *alloc(int64_t length) {
    void *block = std::malloc(length > 0 ? length : 1);
    if (block != nullptr) blocks_.push_back(block);
    return block;
  }
  void free(void *) {}
private:
  std::vector<void *> blocks_;
};
static ObString own_string(ObIAllocator &allocator, const std::string &text) {
  char *buffer = static_cast<char *>(allocator.alloc(text.size() + 1));
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = 0;
  ObString result;
  result.assign_ptr(buffer, text.size());
  return result;
}
static int buffer_creation_count = 0;
static int oversize_buffer_number = -1;
class ObStringBuffer {
public:
  explicit ObStringBuffer(ObIAllocator *allocator)
      : allocator_(allocator), buffer_number_(++buffer_creation_count) {}
  int append(const char *text, uint64_t length) { if (length) text_.append(text, length); return 0; }
  int append(const ObString &value) { return append(value.ptr(), value.length()); }
  uint64_t length() const { return text_.size(); }
  uint64_t capacity() const {
    return buffer_number_ == oversize_buffer_number
        ? static_cast<uint64_t>(INT32_MAX) + 1 : text_.capacity();
  }
  int get_result_string(ObString &result) { result = own_string(*allocator_, text_); return 0; }
private:
  ObIAllocator *allocator_;
  int buffer_number_;
  std::string text_;
};
using ObJsonBuffer = ObStringBuffer;
template<class T> class ObArray {
public:
  int push_back(const T &value) { values_.push_back(value); return 0; }
  int64_t count() const { return values_.size(); }
  T &at(int64_t index) { return values_.at(index); }
  const T &at(int64_t index) const { return values_.at(index); }
private:
  std::vector<T> values_;
};
enum class ObJsonNodeType { J_STRING, J_OBJECT, J_ARRAY, J_NULL, J_INT };
class ObJsonNode {
public:
  virtual ~ObJsonNode() = default;
  virtual ObJsonNodeType json_type() const = 0;
  virtual std::string json() const = 0;
  int print(ObJsonBuffer &buffer, int) const { const std::string text = json(); return buffer.append(text.data(), text.size()); }
};
class ObJsonString : public ObJsonNode {
public:
  explicit ObJsonString(std::string value) : value_(std::move(value)) {}
  ObJsonNodeType json_type() const override { return ObJsonNodeType::J_STRING; }
  ObString get_str() const { ObString value; value.assign_ptr(value_.data(), value_.size()); return value; }
  std::string json() const override { return "\"" + value_ + "\""; }
private:
  std::string value_;
};
class ObJsonNull : public ObJsonNode {
public:
  ObJsonNodeType json_type() const override { return ObJsonNodeType::J_NULL; }
  std::string json() const override { return "null"; }
};
class ObJsonInt : public ObJsonNode {
public:
  ObJsonNodeType json_type() const override { return ObJsonNodeType::J_INT; }
  std::string json() const override { return "1"; }
};
class ObJsonArray : public ObJsonNode {
public:
  ObJsonNodeType json_type() const override { return ObJsonNodeType::J_ARRAY; }
  uint64_t element_count() const { return values_.size(); }
  ObJsonNode *get_value(uint64_t index) const { return index < values_.size() ? values_[index] : nullptr; }
  int append(ObJsonNode *value) { values_.push_back(value); return 0; }
  std::string json() const override {
    std::string result = "[";
    for (auto *value : values_) { if (result.size() > 1) result += ", "; result += value->json(); }
    return result + "]";
  }
private:
  std::vector<ObJsonNode *> values_;
};
class ObJsonObject : public ObJsonNode {
public:
  ObJsonNodeType json_type() const override { return ObJsonNodeType::J_OBJECT; }
  uint64_t element_count() const { return values_.size(); }
  ObJsonNode *get_value(const char *key) const {
    auto found = values_.find(key); return found == values_.end() ? nullptr : found->second;
  }
  void add(const char *key, ObJsonNode *value) { values_[key] = value; }
  std::string json() const override {
    std::string result = "{";
    for (const auto &entry : values_) {
      if (result.size() > 1) result += ", ";
      result += "\"" + entry.first + "\": " + entry.second->json();
    }
    return result + "}";
  }
private:
  std::map<std::string, ObJsonNode *> values_;
};
class ObAIFuncPromptObjectUtils {
public:
  static bool is_valid_prompt_object(ObJsonObject *);
  static int replace_all_str_args_in_template(ObIAllocator &, ObJsonObject *, ObString &);
  static constexpr char prompt_template_key[] = "template";
  static constexpr char prompt_args_key[] = "args";
};

#include "renderer_under_test.inc"

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
static std::string render(ObIAllocator &allocator, ObJsonObject &prompt) {
  ObString result;
  int ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, &prompt, result);
  require(ret == 0, ("renderer returned " + std::to_string(ret)).c_str());
  return std::string(result.ptr() == nullptr ? "" : result.ptr(), result.length());
}
static ObJsonObject envelope(ObJsonString &text, ObJsonArray &args) {
  ObJsonObject prompt; prompt.add("template", &text); prompt.add("args", &args); return prompt;
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "test case name required");
    std::string test = argv[1];
    ObIAllocator allocator;
    if (test == "repeat") {
      ObJsonString text("{0}/{0}/{0}"), value(std::string(1000, 'x'));
      ObJsonArray args; args.append(&value); auto prompt = envelope(text, args);
      require(render(allocator, prompt) == std::string(1000, 'x') + "/" + std::string(1000, 'x') + "/" + std::string(1000, 'x'), "repeated value truncated");
    } else if (test == "overflow") {
      ObJsonString text("{18446744073709551616}"), value("x");
      ObJsonArray args; args.append(&value); auto prompt = envelope(text, args); ObString result;
      require(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, &prompt, result) == OB_INVALID_ARGUMENT, "overflow index accepted");
    } else if (test == "nested_json") {
      ObJsonString inner_text("inner {0}"), value("世界"), outer_text("outer {0} {1}");
      ObJsonArray inner_args; inner_args.append(&value); auto inner = envelope(inner_text, inner_args);
      ObJsonArray list; list.append(&value); ObJsonArray args; args.append(&inner); args.append(&list);
      auto prompt = envelope(outer_text, args);
      require(ObAIFuncPromptObjectUtils::is_valid_prompt_object(&prompt), "JSON array rejected");
      require(render(allocator, prompt) == "outer inner 世界 [\"世界\"]", "nested prompt not rendered");
    } else if (test == "literal_empty") {
      ObJsonString text("中文 {name} {} {0}"), value("");
      ObJsonArray args; args.append(&value); auto prompt = envelope(text, args);
      require(render(allocator, prompt) == "中文 {name} {} ", "literal braces or empty argument changed");
    } else if (test == "json_object") {
      ObJsonString text("{0}"), value("value"); ObJsonObject object; object.add("key", &value);
      ObJsonArray args; args.append(&object); auto prompt = envelope(text, args);
      require(render(allocator, prompt) == "{\"key\": \"value\"}", "ordinary JSON object not serialized");
    } else if (test == "unicode_nul") {
      const std::string raw = std::string("你好\\\"\n") + '\0' + "世界";
      ObJsonString text("{0}/{0}"), value(raw);
      ObJsonArray args; args.append(&value); auto prompt = envelope(text, args);
      require(render(allocator, prompt) == raw + "/" + raw, "UTF-8, escape, or embedded NUL changed");
    } else if (test == "empty_result") {
      ObJsonString text("{0}"), value(""); ObJsonArray args; args.append(&value);
      auto prompt = envelope(text, args);
      require(render(allocator, prompt).empty(), "empty argument result changed");
    } else if (test == "no_arguments") {
      ObJsonString text("plain text"); ObJsonArray args; auto prompt = envelope(text, args);
      require(render(allocator, prompt) == "plain text", "template without arguments changed");
    } else if (test == "capacity_overflow" || test == "json_capacity_overflow") {
      // Model a grown backing buffer without allocating 2 GiB in the harness.
      // The output buffer is constructed first; a structured argument's buffer second.
      oversize_buffer_number = test == "capacity_overflow" ? 1 : 2;
      ObJsonString text("{0}"), value("x"); ObJsonArray args, list; list.append(&value);
      args.append(test == "capacity_overflow" ? static_cast<ObJsonNode *>(&value) : &list);
      auto prompt = envelope(text, args); ObString result;
      require(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, &prompt, result) == OB_SIZE_OVERFLOW,
              "backing capacity exceeding int32 was accepted");
    } else if (test == "malformed") {
      ObJsonArray bad_text, args; ObJsonString value("x"); args.append(&value);
      ObJsonObject prompt; prompt.add("template", &bad_text); prompt.add("args", &args); ObString result;
      require(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, &prompt, result) == OB_INVALID_ARGUMENT, "malformed envelope accepted");
    } else if (test == "malformed_args") {
      ObJsonString text("{0}"), value("x"); ObJsonNull null_args; ObJsonInt number;
      ObJsonArray args; args.append(&value); ObString result;
      ObJsonObject no_args; no_args.add("template", &text);
      ObJsonObject null_array; null_array.add("template", &text); null_array.add("args", &null_args);
      ObJsonObject number_template; number_template.add("template", &number); number_template.add("args", &args);
      for (ObJsonObject *prompt : {&no_args, &null_array, &number_template}) {
        require(!ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt), "malformed envelope validated");
        require(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result) == OB_INVALID_ARGUMENT, "malformed args accepted");
      }
    } else if (test == "depth") {
      ObJsonString text("{0}"), value("x");
      for (const int count : {64, 65}) {
        std::vector<std::unique_ptr<ObJsonArray>> arrays;
        std::vector<std::unique_ptr<ObJsonObject>> objects;
        ObJsonNode *current = &value;
        for (int i = 0; i < count; ++i) {
          arrays.emplace_back(new ObJsonArray); arrays.back()->append(current);
          objects.emplace_back(new ObJsonObject(envelope(text, *arrays.back())));
          current = objects.back().get();
        }
        ObString result;
        const int expected = count == 64 ? OB_SUCCESS : OB_SIZE_OVERFLOW;
        require(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, objects.back().get(), result) == expected, "64/65 prompt depth boundary wrong");
        if (count == 64) require(std::string(result.ptr(), result.length()) == "x", "depth 64 result changed");
      }
    } else {
      throw std::runtime_error("unknown test case");
    }
    std::cout << "PASS " << test << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}

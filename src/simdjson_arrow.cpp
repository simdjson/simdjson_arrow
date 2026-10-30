#include "simdjson_arrow.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace simdjson_arrow {
namespace {

namespace sj = simdjson::ondemand;

// A growable byte buffer from malloc, so that we can hand it over to an
// ArrowArray and free it in the release callback.
class buffer {
 public:
  buffer() = default;
  buffer(const buffer&) = delete;
  buffer& operator=(const buffer&) = delete;
  ~buffer() { std::free(data_); }

  uint8_t* data() { return data_; }
  size_t size() const { return size_; }

  template <typename T>
  void push(T value) {
    reserve(sizeof(T));
    std::memcpy(data_ + size_, &value, sizeof(T));
    size_ += sizeof(T);
  }
  void append(const void* bytes, size_t length) {
    reserve(length);
    if (length != 0) std::memcpy(data_ + size_, bytes, length);
    size_ += length;
  }
  void reserve(size_t additional) {
    if (size_ + additional > capacity_) grow(size_ + additional);
  }
  // Gives up the memory (never null), the buffer is left empty.
  void* release() {
    void* p = data_ != nullptr ? data_ : std::calloc(1, 64);
    if (p == nullptr) throw std::bad_alloc();
    data_ = nullptr;
    size_ = capacity_ = 0;
    return p;
  }

 private:
  void grow(size_t minimum) {
    size_t capacity = capacity_ < 64 ? 64 : capacity_ * 2;
    if (capacity < minimum) capacity = minimum;
    auto* p = static_cast<uint8_t*>(std::realloc(data_, capacity));
    if (p == nullptr) throw std::bad_alloc();
    data_ = p;
    capacity_ = capacity;
  }
  uint8_t* data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

class bitmap {
 public:
  void append(bool value) {
    if ((length_ & 7) == 0) bytes_.push<uint8_t>(0);
    bytes_.data()[length_ >> 3] |= static_cast<uint8_t>(value) << (length_ & 7);
    length_++;
  }
  void* release() {
    length_ = 0;
    return bytes_.release();
  }

 private:
  buffer bytes_;
  int64_t length_{0};
};

// Whether unsafe_is_equal can compare a raw JSON key with this name: the name
// must not contain characters that JSON escapes.
bool is_simple_name(const std::string& name) {
  for (unsigned char c : name) {
    if (c == '"' || c == '\\' || c < 0x20) return false;
  }
  return true;
}

// The builder of one column (one field), with the builders of its children.
struct column {
  const field* f{nullptr};
  column* parent{nullptr};
  type t{type::null};
  bool nullable{true};
  int64_t length{0};
  int64_t null_count{0};
  bitmap validity;  // built only once the column has a null
  bitmap bits;    // values of a boolean column
  buffer values;  // values of a number column, offsets (int32) of a utf8 or list column
  buffer chars;   // bytes of a utf8 column
  std::vector<column> children;
  // For a struct: field lookup, and which fields the current record has.
  std::unordered_map<std::string_view, size_t> index;
  std::vector<uint8_t> simple_name;
  std::vector<uint64_t> seen;
  uint64_t epoch{0};

  void init(const field& schema, column* up) {
    f = &schema;
    parent = up;
    t = schema.type;
    nullable = schema.nullable;
    children = std::vector<column>(schema.children.size());
    for (size_t i = 0; i < children.size(); i++) children[i].init(schema.children[i], this);
    if (schema.type == type::struct_) {
      for (size_t i = 0; i < schema.children.size(); i++) {
        index.emplace(schema.children[i].name, i);
        simple_name.push_back(is_simple_name(schema.children[i].name));
      }
      seen.assign(schema.children.size(), 0);
    }
    start();
  }
  // Starts a new batch.
  void start() {
    length = 0;
    null_count = 0;
    if (t == type::utf8 || t == type::list) values.push<int32_t>(0);
  }
  void append_valid() {
    if (null_count != 0) validity.append(true);
    length++;
  }
  void append_null_bit() {
    if (null_count == 0) {
      for (int64_t i = 0; i < length; i++) validity.append(true);
    }
    validity.append(false);
    null_count++;
    length++;
  }
};

struct array_holder {
  std::vector<void*> owned;
  const void* buffers[3]{nullptr, nullptr, nullptr};
  std::vector<ArrowArray> child_arrays;
  std::vector<ArrowArray*> child_pointers;
};

void release_array(ArrowArray* array) {
  auto* holder = static_cast<array_holder*>(array->private_data);
  for (ArrowArray& child : holder->child_arrays) {
    if (child.release != nullptr) child.release(&child);
  }
  for (void* p : holder->owned) std::free(p);
  delete holder;
  array->release = nullptr;
}

// Hands the column over to out and starts a new batch in the column.
void export_column(column& c, ArrowArray* out) {
  auto* holder = new array_holder;
  int64_t n_buffers = 0;
  auto add = [&](void* p) {
    holder->owned.push_back(p);
    holder->buffers[n_buffers++] = p;
  };
  const type t = c.f->type;
  int64_t null_count = c.null_count;
  if (t == type::null) {
    null_count = c.length;
  } else {
    if (c.null_count == 0) {
      holder->buffers[n_buffers++] = nullptr;
    } else {
      add(c.validity.release());
    }
    switch (t) {
      case type::boolean:
        add(c.bits.release());
        break;
      case type::int64:
      case type::uint64:
      case type::float64:
      case type::list:
        add(c.values.release());
        break;
      case type::utf8:
        add(c.values.release());
        add(c.chars.release());
        break;
      default:
        break;
    }
  }
  holder->child_arrays.resize(c.children.size());
  for (size_t i = 0; i < c.children.size(); i++) {
    export_column(c.children[i], &holder->child_arrays[i]);
    holder->child_pointers.push_back(&holder->child_arrays[i]);
  }
  out->length = c.length;
  out->null_count = null_count;
  out->offset = 0;
  out->n_buffers = n_buffers;
  out->n_children = static_cast<int64_t>(c.children.size());
  out->buffers = holder->buffers;
  out->children = holder->child_pointers.empty() ? nullptr : holder->child_pointers.data();
  out->dictionary = nullptr;
  out->release = release_array;
  out->private_data = holder;
  c.start();
}

const char* type_name(sj::json_type t) {
  switch (t) {
    case sj::json_type::array:
      return "an array";
    case sj::json_type::object:
      return "an object";
    case sj::json_type::number:
      return "a number";
    case sj::json_type::string:
      return "a string";
    case sj::json_type::boolean:
      return "a boolean";
    case sj::json_type::null:
      return "null";
    default:
      return "an invalid value";
  }
}

const char* expected_name(type t) {
  switch (t) {
    case type::null:
      return "null";
    case type::boolean:
      return "a boolean";
    case type::int64:
      return "a 64-bit signed integer";
    case type::uint64:
      return "a 64-bit unsigned integer";
    case type::float64:
      return "a number";
    case type::utf8:
      return "a string";
    case type::struct_:
      return "an object";
    case type::list:
      return "an array";
  }
  return "?";
}

constexpr int kMaxDepth = 1024;

class ndjson_parser {
 public:
  ndjson_parser(const field& schema, const ndjson_options& options) : options_(options) {
    root_.init(schema, nullptr);
  }

  status read(simdjson::padded_string_view json, std::vector<ArrowArray>* batches) {
    if (root_.f->type != type::struct_) return status{"the schema must be a struct"};
    // simdjson needs a window at least as large as the largest record.
    const size_t window = options_.block_size > simdjson::dom::DEFAULT_BATCH_SIZE
                              ? options_.block_size
                              : simdjson::dom::DEFAULT_BATCH_SIZE;
    sj::document_stream stream;
    if (auto e = parser_.iterate_many(json.data(), json.size(), window).get(stream)) {
      return status{std::string("cannot read the JSON: ") + simdjson::error_message(e)};
    }
    size_t batch_start = 0;
    for (auto it = stream.begin(); it != stream.end(); ++it) {
      const size_t position = it.current_index();
      if (root_.length > 0 && position - batch_start >= options_.block_size) {
        flush(batches);
        batch_start = position;
      }
      sj::document_reference document;
      sj::object record;
      if (auto e = (*it).get(document)) return row_error(simdjson::error_message(e));
      if (auto e = document.get_object().get(record)) {
        return row_error(e == simdjson::INCORRECT_TYPE ? "a record must be a JSON object"
                                                       : simdjson::error_message(e));
      }
      if (!parse_struct(root_, record)) return error_status();
      row_++;
    }
    if (stream.truncated_bytes() != 0) return row_error("truncated JSON record");
    if (root_.length > 0) flush(batches);
    return status{};
  }

 private:
  void flush(std::vector<ArrowArray>* batches) {
    batches->emplace_back();
    export_column(root_, &batches->back());
  }

  status row_error(const char* message) {
    return status{std::string(message) + " (row " + std::to_string(row_) + ")"};
  }

  status error_status() {
    std::string path;
    for (const column* c = error_column_; c != nullptr && c->parent != nullptr; c = c->parent) {
      path = (c->parent->f->type == type::list ? "/[]" : "/" + c->f->name) + path;
    }
    return status{error_ + " (row " + std::to_string(row_) + ", field " +
                  (path.empty() ? "/" : path) + ")"};
  }

  bool fail(const column& c, std::string message) {
    error_column_ = &c;
    error_ = std::move(message);
    return false;
  }

  // The value does not have the type of the column: it may not even be valid JSON.
  bool mismatch(const column& c, sj::json_type found, sj::value& value) {
    if (!validate(c, value, 0)) return false;
    return fail(c, std::string("expected ") + expected_name(c.f->type) + ", found " +
                       type_name(found));
  }

  static void append_null(column& c) {
    c.append_null_bit();
    switch (c.t) {
      case type::boolean:
        c.bits.append(false);
        break;
      case type::int64:
      case type::uint64:
      case type::float64:
        c.values.push<uint64_t>(0);
        break;
      case type::utf8:
        c.values.push<int32_t>(static_cast<int32_t>(c.chars.size()));
        break;
      case type::list:
        c.values.push<int32_t>(static_cast<int32_t>(c.children[0].length));
        break;
      case type::struct_:
        for (column& child : c.children) append_null(child);
        break;
      case type::null:
        break;
    }
  }

  // We first try to read a value of the type of the column: the other cases
  // (null, another type, invalid JSON) go to parse_other.
  bool parse_value(column& c, sj::value& value) {
    switch (c.t) {
      case type::boolean: {
        bool b;
        if (simdjson_unlikely(value.get_bool().get(b))) return parse_other(c, value);
        c.bits.append(b);
        break;
      }
      case type::int64: {
        int64_t x;
        if (simdjson_unlikely(value.get_int64().get(x))) return parse_other(c, value);
        c.values.push(x);
        break;
      }
      case type::uint64: {
        uint64_t x;
        if (simdjson_unlikely(value.get_uint64().get(x))) return parse_other(c, value);
        c.values.push(x);
        break;
      }
      case type::float64: {
        double x;
        if (simdjson_unlikely(value.get_double().get(x))) return parse_other(c, value);
        c.values.push(x);
        break;
      }
      case type::utf8: {
        std::string_view s;
        if (simdjson_unlikely(value.get_string().get(s))) return parse_other(c, value);
        if (simdjson_unlikely(c.chars.size() + s.size() > static_cast<size_t>(INT32_MAX))) {
          return fail(c, "more than 2 GB of strings in one batch: use a smaller block_size");
        }
        c.chars.append(s.data(), s.size());
        c.values.push<int32_t>(static_cast<int32_t>(c.chars.size()));
        break;
      }
      case type::struct_: {
        sj::object object;
        if (simdjson_unlikely(value.get_object().get(object))) return parse_other(c, value);
        return parse_struct(c, object);
      }
      case type::list: {
        sj::array array;
        if (simdjson_unlikely(value.get_array().get(array))) return parse_other(c, value);
        return parse_list(c, array);
      }
      case type::null:
        return parse_other(c, value);
    }
    c.append_valid();
    return true;
  }

  // The value is not of the type of the column: it is null, of another type,
  // or invalid.
  simdjson_never_inline bool parse_other(column& c, sj::value& value) {
    sj::json_type json_type;
    if (value.type().get(json_type)) return fail(c, "invalid JSON value");
    if (json_type == sj::json_type::null) {
      bool is_null;
      if (value.is_null().get(is_null) || !is_null) return fail(c, "invalid JSON value");
      if (!c.nullable) return fail(c, "null in a non-nullable field");
      append_null(c);
      return true;
    }
    const bool number_column =
        c.t == type::int64 || c.t == type::uint64 || c.t == type::float64;
    if (number_column && json_type == sj::json_type::number) return number_error(c, value);
    const bool same_type = (c.t == type::boolean && json_type == sj::json_type::boolean) ||
                           (c.t == type::utf8 && json_type == sj::json_type::string) ||
                           (c.t == type::struct_ && json_type == sj::json_type::object) ||
                           (c.t == type::list && json_type == sj::json_type::array);
    if (same_type) return fail(c, "invalid JSON value");
    return mismatch(c, json_type, value);
  }

  bool number_error(const column& c, sj::value& value) {
    std::string_view token = value.raw_json_token();
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' ||
                              token.back() == '\n' || token.back() == '\r')) {
      token.remove_suffix(1);
    }
    return fail(c, std::string("expected ") + expected_name(c.f->type) + ", found " +
                       std::string(token));
  }

  // The index of the field of c named by the key, or c.children.size() if the
  // schema does not have it.
  size_t find_field(column& c, sj::field& f, size_t guess, bool* ok) {
    const size_t n = c.children.size();
    // Keys usually come in the order of the schema.
    if (guess < n && c.simple_name[guess] && f.key().unsafe_is_equal(c.f->children[guess].name)) {
      return guess;
    }
    std::string_view key;
    if (f.unescaped_key().get(key)) {
      *ok = fail(c, "invalid JSON key");
      return n;
    }
    auto it = c.index.find(key);
    return it == c.index.end() ? n : it->second;
  }

  bool parse_struct(column& c, sj::object& object) {
    const uint64_t epoch = ++c.epoch;
    const size_t n = c.children.size();
    size_t guess = 0;
    for (auto field_result : object) {
      sj::field f;
      if (std::move(field_result).get(f)) return fail(c, "invalid JSON object");
      bool ok = true;
      const size_t i = find_field(c, f, guess, &ok);
      if (!ok) return false;
      if (i == n) {
        if (options_.error_on_unknown_fields) return fail(c, "key absent from the schema");
        if (options_.validate_skipped && !validate(c, f.value(), 0)) return false;
        continue;
      }
      if (c.seen[i] == epoch) return fail(c.children[i], "duplicate key");
      c.seen[i] = epoch;
      guess = i + 1;
      if (!parse_value(c.children[i], f.value())) return false;
    }
    for (size_t i = 0; i < n; i++) {
      if (c.seen[i] != epoch) {
        if (!c.children[i].nullable) return fail(c.children[i], "missing non-nullable field");
        append_null(c.children[i]);
      }
    }
    c.append_valid();
    return true;
  }

  bool parse_list(column& c, sj::array& array) {
    column& item = c.children[0];
    for (auto element_result : array) {
      sj::value element;
      if (std::move(element_result).get(element)) return fail(c, "invalid JSON array");
      if (!parse_value(item, element)) return false;
    }
    if (item.length > INT32_MAX) return fail(c, "more than 2^31 list items in one batch");
    c.values.push<int32_t>(static_cast<int32_t>(item.length));
    c.append_valid();
    return true;
  }

  // Checks that a skipped value is valid JSON. Errors are reported on the
  // struct column c that skips it.
  bool validate(const column& c, sj::value value, int depth) {
    if (depth > kMaxDepth) return fail(c, "JSON too deeply nested");
    sj::json_type json_type;
    if (value.type().get(json_type)) return fail(c, "invalid JSON value");
    switch (json_type) {
      case sj::json_type::null: {
        bool is_null;
        if (value.is_null().get(is_null) || !is_null) return fail(c, "invalid JSON value");
        return true;
      }
      case sj::json_type::boolean: {
        bool b;
        if (value.get_bool().get(b)) return fail(c, "invalid JSON value");
        return true;
      }
      case sj::json_type::number: {
        sj::number number;
        auto e = value.get_number().get(number);
        if (e && e != simdjson::BIGINT_ERROR) return fail(c, "invalid JSON number");
        return true;
      }
      case sj::json_type::string: {
        std::string_view s;
        if (value.get_string().get(s)) return fail(c, "invalid JSON string");
        return true;
      }
      case sj::json_type::array: {
        sj::array array;
        if (value.get_array().get(array)) return fail(c, "invalid JSON array");
        for (auto element_result : array) {
          sj::value element;
          if (std::move(element_result).get(element)) return fail(c, "invalid JSON array");
          if (!validate(c, element, depth + 1)) return false;
        }
        return true;
      }
      case sj::json_type::object: {
        sj::object object;
        if (value.get_object().get(object)) return fail(c, "invalid JSON object");
        for (auto field_result : object) {
          sj::field f;
          std::string_view key;
          if (std::move(field_result).get(f) || f.unescaped_key().get(key)) {
            return fail(c, "invalid JSON object");
          }
          if (!validate(c, f.value(), depth + 1)) return false;
        }
        return true;
      }
      default:
        return fail(c, "invalid JSON value");
    }
  }

  ndjson_options options_;
  sj::parser parser_;
  column root_;
  int64_t row_{0};
  std::string error_;
  const column* error_column_{nullptr};
};

struct schema_holder {
  std::string format;
  std::string name;
  std::vector<ArrowSchema> child_schemas;
  std::vector<ArrowSchema*> child_pointers;
};

void release_schema(ArrowSchema* schema) {
  auto* holder = static_cast<schema_holder*>(schema->private_data);
  for (ArrowSchema& child : holder->child_schemas) {
    if (child.release != nullptr) child.release(&child);
  }
  delete holder;
  schema->release = nullptr;
}

const char* format_of(type t) {
  switch (t) {
    case type::null:
      return "n";
    case type::boolean:
      return "b";
    case type::int64:
      return "l";
    case type::uint64:
      return "L";
    case type::float64:
      return "g";
    case type::utf8:
      return "u";
    case type::struct_:
      return "+s";
    case type::list:
      return "+l";
  }
  return "";
}

struct stream_holder {
  field schema;
  std::deque<ArrowArray> batches;
  std::string last_error;
};

int stream_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
  auto* holder = static_cast<stream_holder*>(stream->private_data);
  status s = export_schema(holder->schema, out);
  if (!s.ok()) {
    holder->last_error = s.message;
    return EINVAL;
  }
  return 0;
}

int stream_get_next(ArrowArrayStream* stream, ArrowArray* out) {
  auto* holder = static_cast<stream_holder*>(stream->private_data);
  if (holder->batches.empty()) {
    out->release = nullptr;  // end of the stream
    return 0;
  }
  *out = holder->batches.front();
  holder->batches.pop_front();
  return 0;
}

const char* stream_get_last_error(ArrowArrayStream* stream) {
  auto* holder = static_cast<stream_holder*>(stream->private_data);
  return holder->last_error.empty() ? nullptr : holder->last_error.c_str();
}

void stream_release(ArrowArrayStream* stream) {
  auto* holder = static_cast<stream_holder*>(stream->private_data);
  for (ArrowArray& batch : holder->batches) {
    if (batch.release != nullptr) batch.release(&batch);
  }
  delete holder;
  stream->release = nullptr;
}

}  // namespace

status import_schema(const ArrowSchema* schema, field* out) {
  if (schema == nullptr || schema->format == nullptr) return status{"invalid ArrowSchema"};
  const std::string format = schema->format;
  field f;
  f.name = schema->name != nullptr ? schema->name : "";
  f.nullable = (schema->flags & ARROW_FLAG_NULLABLE) != 0;
  if (format == "n") {
    f.type = type::null;
  } else if (format == "b") {
    f.type = type::boolean;
  } else if (format == "l") {
    f.type = type::int64;
  } else if (format == "L") {
    f.type = type::uint64;
  } else if (format == "g") {
    f.type = type::float64;
  } else if (format == "u") {
    f.type = type::utf8;
  } else if (format == "+s") {
    f.type = type::struct_;
  } else if (format == "+l") {
    f.type = type::list;
  } else {
    return status{"unsupported Arrow type (format \"" + format + "\") for field " + f.name};
  }
  if (schema->dictionary != nullptr) return status{"dictionary types are not supported"};
  if ((f.type == type::list && schema->n_children != 1) ||
      (f.type != type::list && f.type != type::struct_ && schema->n_children != 0)) {
    return status{"invalid ArrowSchema for field " + f.name};
  }
  for (int64_t i = 0; i < schema->n_children; i++) {
    field child;
    status s = import_schema(schema->children[i], &child);
    if (!s.ok()) return s;
    f.children.push_back(std::move(child));
  }
  *out = std::move(f);
  return status{};
}

status export_schema(const field& schema, ArrowSchema* out) {
  auto* holder = new schema_holder;
  holder->format = format_of(schema.type);
  holder->name = schema.name;
  holder->child_schemas.resize(schema.children.size());
  for (size_t i = 0; i < schema.children.size(); i++) {
    status s = export_schema(schema.children[i], &holder->child_schemas[i]);
    if (!s.ok()) {
      for (size_t j = 0; j < i; j++) holder->child_schemas[j].release(&holder->child_schemas[j]);
      delete holder;
      return s;
    }
    holder->child_pointers.push_back(&holder->child_schemas[i]);
  }
  out->format = holder->format.c_str();
  out->name = holder->name.c_str();
  out->metadata = nullptr;
  out->flags = schema.nullable ? ARROW_FLAG_NULLABLE : 0;
  out->n_children = static_cast<int64_t>(schema.children.size());
  out->children = holder->child_pointers.empty() ? nullptr : holder->child_pointers.data();
  out->dictionary = nullptr;
  out->release = release_schema;
  out->private_data = holder;
  return status{};
}

status read_ndjson(const field& schema, simdjson::padded_string_view json,
                   std::vector<ArrowArray>* batches, const ndjson_options& options) {
  const size_t first = batches->size();
  status s;
  try {
    ndjson_parser parser(schema, options);
    s = parser.read(json, batches);
  } catch (const std::bad_alloc&) {
    s = status{"out of memory"};
  }
  if (!s.ok()) {
    for (size_t i = first; i < batches->size(); i++) (*batches)[i].release(&(*batches)[i]);
    batches->resize(first);
  }
  return s;
}

void make_stream(const field& schema, std::vector<ArrowArray>&& batches, ArrowArrayStream* out) {
  auto* holder = new stream_holder;
  holder->schema = schema;
  for (ArrowArray& batch : batches) holder->batches.push_back(batch);
  batches.clear();
  out->get_schema = stream_get_schema;
  out->get_next = stream_get_next;
  out->get_last_error = stream_get_last_error;
  out->release = stream_release;
  out->private_data = holder;
}

}  // namespace simdjson_arrow

extern "C" int simdjson_arrow_read_ndjson(const char* data, size_t length, size_t capacity,
                                          const struct ArrowSchema* schema, size_t block_size,
                                          int error_on_unknown_fields, int validate_skipped,
                                          struct ArrowArrayStream* out, char* error,
                                          size_t error_capacity) {
  using namespace simdjson_arrow;
  auto report = [&](const std::string& message) {
    if (error != nullptr && error_capacity > 0) {
      size_t n = message.size() < error_capacity - 1 ? message.size() : error_capacity - 1;
      std::memcpy(error, message.data(), n);
      error[n] = '\0';
    }
    return 1;
  };
  try {
    field f;
    status s = import_schema(schema, &f);
    if (!s.ok()) return report(s.message);
    ndjson_options options;
    if (block_size != 0) options.block_size = block_size;
    options.error_on_unknown_fields = error_on_unknown_fields != 0;
    options.validate_skipped = validate_skipped != 0;
    std::vector<ArrowArray> batches;
    if (capacity >= length + simdjson::SIMDJSON_PADDING) {
      s = read_ndjson(f, simdjson::padded_string_view(data, length, capacity), &batches, options);
    } else {
      simdjson::padded_string copy(data, length);
      s = read_ndjson(f, copy, &batches, options);
    }
    if (!s.ok()) return report(s.message);
    make_stream(f, std::move(batches), out);
    return 0;
  } catch (const std::bad_alloc&) {
    return report("out of memory");
  }
}

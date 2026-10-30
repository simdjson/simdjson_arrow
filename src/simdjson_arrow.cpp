#include "simdjson_arrow.h"

#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
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
  void append_zeros(size_t length) {
    reserve(length);
    if (length != 0) std::memset(data_ + size_, 0, length);
    size_ += length;
  }
  void reserve(size_t additional) {
    if (size_ + additional > capacity_) grow(size_ + additional);
  }
  // Counts as written the bytes after the end (which must be reserved).
  void advance(size_t length) { size_ += length; }
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
  void append_zeros(int64_t n) {
    // The bits past the end are zero: we only need more bytes.
    const size_t bytes = static_cast<size_t>((length_ + n + 7) >> 3);
    if (bytes > bytes_.size()) bytes_.append_zeros(bytes - bytes_.size());
    length_ += n;
  }
  void append_ones(int64_t n) {
    for (; n > 0 && (length_ & 7) != 0; n--) append(true);
    if (n >= 8) {
      const size_t full = static_cast<size_t>(n >> 3);
      bytes_.reserve(full);
      std::memset(bytes_.data() + bytes_.size(), 0xff, full);
      advance(full);
      length_ += static_cast<int64_t>(full) * 8;
      n &= 7;
    }
    for (; n > 0; n--) append(true);
  }
  void* release() {
    length_ = 0;
    return bytes_.release();
  }

 private:
  void advance(size_t bytes) { bytes_.advance(bytes); }
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
// When we infer the schema, columns are added, and their types change, as we go.
struct column {
  std::string name;
  column* parent{nullptr};
  type t{type::null};
  bool nullable{true};
  int64_t length{0};      // values written
  int64_t null_count{0};  // among the values written
  int64_t pending{0};     // nulls that follow, not written yet (we write them in bulk)
  bitmap validity;  // built only once the column has a null
  bitmap bits;      // values of a boolean column
  buffer values;    // values of a number column, offsets (int32) of a utf8 or list column
  buffer chars;     // bytes of a utf8 column
  std::vector<std::unique_ptr<column>> children;
  // For a struct: field lookup (index is built when we first need it), and
  // which fields the current record has.
  std::unordered_map<std::string_view, size_t> index;
  bool indexed{false};
  std::vector<uint8_t> simple_name;
  std::vector<uint64_t> seen;
  uint64_t epoch{0};

  column(std::string n, type ty, bool null_ok, column* up)
      : name(std::move(n)), parent(up), t(ty), nullable(null_ok) {
    start();
  }

  // An empty column for the field (and its children).
  static std::unique_ptr<column> make(const field& f, column* up) {
    auto c = std::make_unique<column>(f.name, f.type, f.nullable, up);
    for (const field& child : f.children) c->adopt(make(child, c.get()));
    return c;
  }
  // An empty column with the same fields and types.
  std::unique_ptr<column> clone_structure(column* up) const {
    auto c = std::make_unique<column>(name, t, nullable, up);
    for (const auto& child : children) c->adopt(child->clone_structure(c.get()));
    return c;
  }
  // Adds a child (the last one).
  column* adopt(std::unique_ptr<column> child) {
    child->parent = this;
    children.push_back(std::move(child));
    const column* added = children.back().get();
    if (indexed) index.emplace(added->name, children.size() - 1);
    simple_name.push_back(is_simple_name(added->name));
    seen.push_back(0);
    return children.back().get();
  }
  // The index of the child with this name, or children.size().
  size_t find(std::string_view key) {
    if (!indexed) {
      for (size_t i = 0; i < children.size(); i++) index.emplace(children[i]->name, i);
      indexed = true;
    }
    auto it = index.find(key);
    return it == index.end() ? children.size() : it->second;
  }
  void reindex() {
    index.clear();
    indexed = false;
    simple_name.clear();
    for (const auto& child : children) simple_name.push_back(is_simple_name(child->name));
    seen.assign(children.size(), 0);
  }
  // The number of values, written or pending.
  int64_t size() const { return length + pending; }
  // Starts a new batch.
  void start() {
    length = 0;
    null_count = 0;
    pending = 0;
    if (t == type::utf8 || t == type::list) values.push<int32_t>(0);
  }
  // After the pending nulls are written.
  void append_valid() {
    if (null_count != 0) validity.append(true);
    length++;
  }
  // The field we built (the schema of the column).
  field describe() const {
    field f{name, t, nullable, {}};
    for (const auto& child : children) f.children.push_back(child->describe());
    return f;
  }
};

void append_null(column& c) { c.pending++; }

// Writes the pending nulls of the column.
void write_nulls(column& c) {
  const int64_t n = c.pending;
  if (n == 0) return;
  c.pending = 0;
  if (c.t != type::null) {
    if (c.null_count == 0) c.validity.append_ones(c.length);
    c.validity.append_zeros(n);
  }
  c.null_count += n;
  c.length += n;
  switch (c.t) {
    case type::boolean:
      c.bits.append_zeros(n);
      break;
    case type::int64:
    case type::uint64:
    case type::float64:
      c.values.append_zeros(static_cast<size_t>(n) * 8);
      break;
    case type::utf8:
      for (int64_t i = 0; i < n; i++) c.values.push<int32_t>(static_cast<int32_t>(c.chars.size()));
      break;
    case type::list: {
      const int32_t end = static_cast<int32_t>(c.children[0]->size());
      for (int64_t i = 0; i < n; i++) c.values.push<int32_t>(end);
      break;
    }
    case type::struct_:
      for (auto& child : c.children) child->pending += n;
      break;
    case type::null:
      break;
  }
}

// Gives a null column (only nulls so far) the type t.
void become(column& c, type t) {
  const int64_t nulls = c.size();
  c.t = t;
  c.start();
  if (t == type::list) c.adopt(std::make_unique<column>("item", type::null, true, &c));
  c.pending = nulls;
}

// Turns an int64 column into a float64 column.
void to_double(column& c) {
  uint8_t* p = c.values.data();
  for (size_t i = 0; i + 8 <= c.values.size(); i += 8) {
    int64_t x;
    std::memcpy(&x, p + i, 8);
    const double d = static_cast<double>(x);
    std::memcpy(p + i, &d, 8);
  }
  c.t = type::float64;
}

const char* type_word(type t) {
  switch (t) {
    case type::null:
      return "null";
    case type::boolean:
      return "boolean";
    case type::int64:
      return "int64";
    case type::uint64:
      return "uint64";
    case type::float64:
      return "float64";
    case type::utf8:
      return "string";
    case type::struct_:
      return "struct";
    case type::list:
      return "list";
  }
  return "?";
}

// Merges the field from into the field into (the schemas of two batches):
// null gives way to any type, int64 to float64, and the fields of structs are
// united (in the order in which they appear).
status merge(field& into, const field& from, const std::string& path) {
  if (from.type == type::null) return status{};
  if (into.type == type::null) {
    into.type = from.type;
    into.children = from.children;
    return status{};
  }
  if (into.type != from.type) {
    if ((into.type == type::int64 || into.type == type::float64) &&
        (from.type == type::int64 || from.type == type::float64)) {
      into.type = type::float64;
      return status{};
    }
    return status{"the field " + (path.empty() ? std::string("/") : path) + " has values of type " +
                  type_word(into.type) + " and of type " + type_word(from.type)};
  }
  if (into.type == type::list) {
    return merge(into.children[0], from.children[0], path + "/[]");
  }
  if (into.type == type::struct_) {
    for (const field& child : from.children) {
      auto it = std::find_if(into.children.begin(), into.children.end(),
                             [&](const field& f) { return f.name == child.name; });
      if (it == into.children.end()) {
        into.children.push_back(child);
      } else {
        status s = merge(*it, child, path + "/" + child.name);
        if (!s.ok()) return s;
      }
    }
  }
  return status{};
}

// Makes the column (of a batch) follow the field (of the whole table), which
// merges the schema of the column with others.
void conform(column& c, const field& f) {
  if (c.t == type::null && f.type != type::null) become(c, f.type);
  if (c.t == type::int64 && f.type == type::float64) to_double(c);
  if (f.type == type::list) {
    conform(*c.children[0], f.children[0]);
  } else if (f.type == type::struct_) {
    std::vector<std::unique_ptr<column>> kids;
    for (const field& child : f.children) {
      const size_t i = c.find(child.name);
      std::unique_ptr<column> kid;
      if (i != c.children.size()) {
        kid = std::move(c.children[i]);
      } else {  // a field absent from this batch: all nulls
        kid = std::make_unique<column>(child.name, type::null, true, &c);
        kid->pending = c.size();
      }
      conform(*kid, child);
      kids.push_back(std::move(kid));
    }
    c.children = std::move(kids);
    c.reindex();
  }
}

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

// Hands the data of the column over to out (the column is left empty).
void export_column(column& c, ArrowArray* out) {
  write_nulls(c);
  auto* holder = new array_holder;
  int64_t n_buffers = 0;
  auto add = [&](void* p) {
    holder->owned.push_back(p);
    holder->buffers[n_buffers++] = p;
  };
  const type t = c.t;
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
    export_column(*c.children[i], &holder->child_arrays[i]);
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

// The records of a part of the input, read into columns (a batch).
struct piece {
  size_t start{0};  // where the part begins in the input
  std::unique_ptr<column> root;
};

class ndjson_parser {
 public:
  // With a null schema, we infer it.
  ndjson_parser(const field* schema, const ndjson_options& options)
      : options_(options), infer_(schema == nullptr), schema_(schema) {}

  // Reads all of the input, in batches of about block_size bytes.
  status read(simdjson::padded_string_view json, std::vector<piece>* pieces) {
    if (schema_ != nullptr && schema_->type != type::struct_) {
      return status{"the schema must be a struct"};
    }
    sj::document_stream stream;
    if (auto e = parser_.iterate_many(json.data(), json.size(), window(options_.block_size))
                     .get(stream)) {
      return status{std::string("cannot read the JSON: ") + simdjson::error_message(e)};
    }
    new_root(nullptr);
    size_t batch_start = 0;
    for (auto it = stream.begin(); it != stream.end(); ++it) {
      const size_t position = it.current_index();
      if (root_->length > 0 && position - batch_start >= options_.block_size) {
        pieces->push_back(piece{batch_start, std::move(root_)});
        new_root(pieces->back().root.get());
        batch_start = position;
      }
      if (!parse_record(*it)) return error_status();
    }
    if (stream.truncated_bytes() != 0) return row_error("truncated JSON record");
    if (root_->length > 0) pieces->push_back(piece{batch_start, std::move(root_)});
    return status{};
  }

  // Reads the records in json[0, length), which ends at a line end, into one
  // batch; start is where it begins in the input. Returns false on error (we
  // then read the input again in one thread, for the error message).
  bool read_slice(const char* json, size_t length, size_t start, std::vector<piece>* pieces) {
    sj::document_stream stream;
    if (parser_.iterate_many(json, length, window(length)).get(stream)) return false;
    new_root(pieces->empty() ? nullptr : pieces->back().root.get());
    for (auto it = stream.begin(); it != stream.end(); ++it) {
      if (!parse_record(*it)) return false;
    }
    if (stream.truncated_bytes() != 0) return false;
    if (root_->length > 0) pieces->push_back(piece{start, std::move(root_)});
    return true;
  }

 private:
  // simdjson needs a window at least as large as the largest record.
  static size_t window(size_t size) {
    return size > simdjson::dom::DEFAULT_BATCH_SIZE ? size : simdjson::dom::DEFAULT_BATCH_SIZE;
  }

  // Starts a batch, with the fields of the previous batch if we infer the schema.
  void new_root(const column* previous) {
    if (!infer_) {
      root_ = column::make(*schema_, nullptr);
    } else if (previous != nullptr) {
      root_ = previous->clone_structure(nullptr);
    } else {
      root_ = std::make_unique<column>("", type::struct_, false, nullptr);
    }
  }

  bool parse_record(simdjson::simdjson_result<sj::document_reference> result) {
    sj::document_reference document;
    sj::object record;
    if (auto e = std::move(result).get(document)) return record_error(simdjson::error_message(e));
    if (auto e = document.get_object().get(record)) {
      return record_error(e == simdjson::INCORRECT_TYPE ? "a record must be a JSON object"
                                                        : simdjson::error_message(e));
    }
    if (!parse_struct(*root_, record)) return false;
    row_++;
    return true;
  }

  bool record_error(const char* message) {
    error_column_ = nullptr;
    error_ = message;
    return false;
  }

  status row_error(const char* message) {
    return status{std::string(message) + " (row " + std::to_string(row_) + ")"};
  }

  status error_status() {
    if (error_column_ == nullptr) return row_error(error_.c_str());
    std::string path;
    for (const column* c = error_column_; c != nullptr && c->parent != nullptr; c = c->parent) {
      path = (c->parent->t == type::list ? "/[]" : "/" + c->name) + path;
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
    return fail(c, std::string("expected ") + expected_name(c.t) + ", found " +
                       type_name(found));
  }

  // We first try to read a value of the type of the column: the other cases
  // (null, another type, invalid JSON) go to parse_other.
  bool parse_value(column& c, sj::value& value) {
    if (simdjson_unlikely(c.pending != 0)) write_nulls(c);
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
    if (infer_) {
      // A column of nulls takes the type of its first value; an int64 column
      // becomes a float64 column when it meets another number.
      if (c.t == type::null) {
        type t;
        if (!infer_type(c, json_type, value, &t)) return false;
        become(c, t);
        return parse_value(c, value);
      }
      if (c.t == type::int64 && json_type == sj::json_type::number) {
        sj::number_type number_type;
        if (!value.get_number_type().get(number_type) &&
            number_type != sj::number_type::signed_integer) {
          to_double(c);
          return parse_value(c, value);
        }
      }
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

  bool infer_type(const column& c, sj::json_type json_type, sj::value& value, type* out) {
    switch (json_type) {
      case sj::json_type::boolean:
        *out = type::boolean;
        return true;
      case sj::json_type::number: {
        // Integers that fit in an int64 are int64, other numbers are float64.
        sj::number_type number_type;
        *out = !value.get_number_type().get(number_type) &&
                       number_type != sj::number_type::signed_integer
                   ? type::float64
                   : type::int64;
        return true;
      }
      case sj::json_type::string:
        *out = type::utf8;
        return true;
      case sj::json_type::object:
        *out = type::struct_;
        return true;
      case sj::json_type::array:
        *out = type::list;
        return true;
      default:
        return fail(c, "invalid JSON value");
    }
  }

  bool number_error(const column& c, sj::value& value) {
    std::string_view token = value.raw_json_token();
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' ||
                              token.back() == '\n' || token.back() == '\r')) {
      token.remove_suffix(1);
    }
    return fail(c, std::string("expected ") + expected_name(c.t) + ", found " +
                       std::string(token));
  }

  // The index of the field of c named by the key, or c.children.size() if the
  // schema does not have it (then *key is the unescaped key).
  size_t find_field(column& c, sj::field& f, size_t guess, std::string_view* key, bool* ok) {
    const size_t n = c.children.size();
    // Keys usually come in the order of the schema.
    if (guess < n && c.simple_name[guess] && f.key().unsafe_is_equal(c.children[guess]->name)) {
      return guess;
    }
    if (f.unescaped_key().get(*key)) {
      *ok = fail(c, "invalid JSON key");
      return n;
    }
    return c.find(*key);
  }

  bool parse_struct(column& c, sj::object& object) {
    const uint64_t epoch = ++c.epoch;
    size_t guess = 0;
    for (auto field_result : object) {
      sj::field f;
      if (std::move(field_result).get(f)) return fail(c, "invalid JSON object");
      bool ok = true;
      std::string_view key;
      size_t i = find_field(c, f, guess, &key, &ok);
      if (!ok) return false;
      if (i == c.children.size()) {
        if (infer_) {  // a new field, null in the previous records
          column* added = c.adopt(std::make_unique<column>(std::string(key), type::null, true, &c));
          added->pending = c.size();
        } else {
          if (options_.error_on_unknown_fields) return fail(c, "key absent from the schema");
          if (options_.validate_skipped && !validate(c, f.value(), 0)) return false;
          continue;
        }
      }
      if (c.seen[i] == epoch) return fail(*c.children[i], "duplicate key");
      c.seen[i] = epoch;
      guess = i + 1;
      if (!parse_value(*c.children[i], f.value())) return false;
    }
    for (size_t i = 0; i < c.children.size(); i++) {
      if (c.seen[i] != epoch) {
        if (!c.children[i]->nullable) return fail(*c.children[i], "missing non-nullable field");
        append_null(*c.children[i]);
      }
    }
    c.append_valid();
    return true;
  }

  bool parse_list(column& c, sj::array& array) {
    column& item = *c.children[0];
    for (auto element_result : array) {
      sj::value element;
      if (std::move(element_result).get(element)) return fail(c, "invalid JSON array");
      if (!parse_value(item, element)) return false;
    }
    if (item.size() > INT32_MAX) return fail(c, "more than 2^31 list items in one batch");
    c.values.push<int32_t>(static_cast<int32_t>(item.size()));
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
  bool infer_;
  const field* schema_;
  sj::parser parser_;
  std::unique_ptr<column> root_;
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

namespace {

using internal::parallel_for;

// The first line start at or after x (or the end of the input).
size_t snap(const char* json, size_t size, size_t x) {
  if (x == 0) return 0;
  if (x >= size) return size;
  const void* p = std::memchr(json + x - 1, '\n', size - (x - 1));
  return p == nullptr ? size : static_cast<const char*>(p) - json + 1;
}

// Reads the input json[0, size) in blocks, with threads. Each thread takes the
// next block of the input from a shared counter and moves both ends of the
// block forward to a line start, so that the blocks are adjacent and hold
// whole lines. simdjson may read SIMDJSON_PADDING bytes past the end of a
// block: if the input is not padded, we copy the blocks that end closer than
// that to the end of the input. Returns false if a block could not be read.
bool read_blocks(const field* schema, const char* json, size_t size, bool padded,
                 const ndjson_options& options, std::vector<piece>* pieces) {
  if (size == 0) return true;
  const size_t block = options.block_size == 0 ? 1 : options.block_size;
  const size_t blocks = (size + block - 1) / block;
  const size_t n_threads = std::min(std::max<size_t>(options.threads, 1), blocks);
  (void)simdjson::get_active_implementation();  // select the implementation once
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::vector<std::vector<piece>> results(n_threads);
  auto work = [&](size_t t) {
    try {
      ndjson_parser parser(schema, options);
      while (!failed.load(std::memory_order_relaxed)) {
        const size_t claim = next.fetch_add(block, std::memory_order_relaxed);
        if (claim >= size) break;
        const size_t begin = snap(json, size, claim);
        const size_t end = snap(json, size, claim + block);
        if (begin == end) continue;
        bool ok;
        if (!padded && end + simdjson::SIMDJSON_PADDING > size) {
          simdjson::padded_string last(json + begin, end - begin);
          ok = parser.read_slice(last.data(), last.size(), begin, &results[t]);
        } else {
          ok = parser.read_slice(json + begin, end - begin, begin, &results[t]);
        }
        if (!ok) failed = true;
      }
    } catch (const std::bad_alloc&) {
      failed = true;
    }
  };
  std::vector<std::thread> threads;
  for (size_t t = 1; t < n_threads; t++) threads.emplace_back(work, t);
  work(0);
  for (std::thread& t : threads) t.join();
  if (failed) return false;
  for (auto& r : results) {
    for (piece& p : r) pieces->push_back(std::move(p));
  }
  std::sort(pieces->begin(), pieces->end(),
            [](const piece& a, const piece& b) { return a.start < b.start; });
  return true;
}

// Reads the input json[0, size) into batches; with a null schema, we infer it
// into *inferred. If padded, SIMDJSON_PADDING bytes follow the input.
status read_batches(const field* schema, const char* json, size_t size, bool padded,
                    std::vector<ArrowArray>* batches, field* inferred,
                    const ndjson_options& options) {
  std::vector<piece> pieces;
  bool done = false;
  // Without padding, we read in blocks even with one thread: we copy only the
  // last block.
  if ((options.threads > 1 && size > options.block_size) || !padded) {
    done = read_blocks(schema, json, size, padded, options, &pieces);
    if (!done) pieces.clear();
  }
  if (!done) {  // one thread, or an error (we read again for the error message)
    ndjson_parser parser(schema, options);
    status s;
    if (padded) {
      s = parser.read(simdjson::padded_string_view(json, size, size + simdjson::SIMDJSON_PADDING),
                      &pieces);
    } else {
      simdjson::padded_string copy(json, size);
      s = parser.read(copy, &pieces);
    }
    if (!s.ok()) return s;
  }
  // The batches are independent: we finish them with all the threads.
  const size_t threads = std::max<size_t>(options.threads, 1);
  if (schema == nullptr) {
    std::vector<field> fields(pieces.size());
    parallel_for(pieces.size(), threads, [&](size_t i) { fields[i] = pieces[i].root->describe(); });
    field table{"", type::struct_, false, {}};
    for (const field& f : fields) {
      status s = merge(table, f, "");
      if (!s.ok()) return s;
    }
    parallel_for(pieces.size(), threads, [&](size_t i) { conform(*pieces[i].root, table); });
    *inferred = std::move(table);
  }
  const size_t first = batches->size();
  batches->resize(first + pieces.size());
  for (size_t i = first; i < batches->size(); i++) (*batches)[i].release = nullptr;
  parallel_for(pieces.size(), threads,
               [&](size_t i) { export_column(*pieces[i].root, &(*batches)[first + i]); });
  return status{};
}

status read_guarded(const field* schema, const char* json, size_t size, bool padded,
                    std::vector<ArrowArray>* batches, field* inferred,
                    const ndjson_options& options) {
  const size_t first = batches->size();
  status s;
  try {
    s = read_batches(schema, json, size, padded, batches, inferred, options);
  } catch (const std::bad_alloc&) {
    s = status{"out of memory"};
  }
  if (!s.ok()) {
    for (size_t i = first; i < batches->size(); i++) (*batches)[i].release(&(*batches)[i]);
    batches->resize(first);
  }
  return s;
}

}  // namespace

status read_ndjson(const field& schema, simdjson::padded_string_view json,
                   std::vector<ArrowArray>* batches, const ndjson_options& options) {
  return read_guarded(&schema, json.data(), json.size(), true, batches, nullptr, options);
}

status read_ndjson(simdjson::padded_string_view json, field* schema,
                   std::vector<ArrowArray>* batches, const ndjson_options& options) {
  return read_guarded(nullptr, json.data(), json.size(), true, batches, schema, options);
}

status read_ndjson(const field& schema, const char* json, size_t length,
                   std::vector<ArrowArray>* batches, const ndjson_options& options) {
  return read_guarded(&schema, json, length, false, batches, nullptr, options);
}

status read_ndjson(const char* json, size_t length, field* schema,
                   std::vector<ArrowArray>* batches, const ndjson_options& options) {
  return read_guarded(nullptr, json, length, false, batches, schema, options);
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
                                          size_t threads, struct ArrowArrayStream* out,
                                          char* error, size_t error_capacity) {
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
    status s;
    if (schema != nullptr) {
      s = import_schema(schema, &f);
      if (!s.ok()) return report(s.message);
    }
    ndjson_options options;
    if (block_size != 0) options.block_size = block_size;
    options.error_on_unknown_fields = error_on_unknown_fields != 0;
    options.validate_skipped = validate_skipped != 0;
    options.threads = threads == 0 ? 1 : threads;
    std::vector<ArrowArray> batches;
    if (capacity >= length + simdjson::SIMDJSON_PADDING) {
      const simdjson::padded_string_view json(data, length, capacity);
      s = schema != nullptr ? read_ndjson(f, json, &batches, options)
                            : read_ndjson(json, &f, &batches, options);
    } else {
      s = schema != nullptr ? read_ndjson(f, data, length, &batches, options)
                            : read_ndjson(data, length, &f, &batches, options);
    }
    if (!s.ok()) return report(s.message);
    make_stream(f, std::move(batches), out);
    return 0;
  } catch (const std::bad_alloc&) {
    return report("out of memory");
  }
}

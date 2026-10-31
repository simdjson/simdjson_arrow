// Writes Arrow record batches as newline-delimited JSON.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "simdjson_arrow.h"

#include "parallel.h"
#include "zmij.h"  // shortest representation of floating-point numbers

namespace simdjson_arrow {
namespace {

// The JSON we write: a buffer from malloc (or from an allocator), which we can
// hand over without a copy. It grows without being zeroed (unlike a
// std::string).
class output {
 public:
  output() = default;
  explicit output(const ndjson_allocator* allocator) : allocator_(allocator) {}
  output(const output&) = delete;
  output& operator=(const output&) = delete;
  ~output() {
    if (allocator_ == nullptr) {
      std::free(data_);
    } else if (data_ != nullptr) {
      allocator_->deallocate(allocator_->context, data_);
    }
  }

  size_t size() const { return size_; }
  std::string_view view() const { return std::string_view(data_, size_); }

  // Room for n more bytes.
  char* reserve(size_t n) {
    if (simdjson_unlikely(capacity_ - size_ < n)) grow(n);
    return data_ + size_;
  }
  void append(char c) {
    *reserve(1) = c;
    size_++;
  }
  void append_raw(std::string_view s) {
    std::memcpy(reserve(s.size()), s.data(), s.size());
    size_ += s.size();
  }
  void append_null() { append_raw("null"); }
  void append_comma() { append(','); }
  void append_colon() { append(':'); }
  void append_bool(bool b) { append_raw(b ? std::string_view("true") : std::string_view("false")); }
  void append_uint(uint64_t x) {
    char* p = reserve(20);
    size_ = simdjson::internal::write_uint_jeaiii(p, x) - data_;
  }
  void append_int(int64_t x) {
    char* p = reserve(20);
    const bool negative = x < 0;
    *p = '-';
    p += negative;
    const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(x) : static_cast<uint64_t>(x);
    size_ = simdjson::internal::write_uint_jeaiii(p, magnitude) - data_;
  }
  // x must be finite. We write the shortest representation that gives back x,
  // with a decimal point or an exponent (1.0, not 1), so that the number reads
  // back as a floating-point number.
  void append_float(double x) {
    constexpr size_t room = 40;  // zmij needs at most 34 bytes, ".0" 2 more
    char* p = reserve(room);
    char* end = zmij::write(p, room, x);
    // zmij writes fixed notation for 1e-4 <= |x| < 1e16 (like Python's repr),
    // without a decimal point for integers.
    if (x == std::floor(x) && std::fabs(x) < 1e16) {
      std::memcpy(end, ".0", 2);
      end += 2;
    }
    size_ = end - data_;
  }
  void append_float(float x) {
    constexpr size_t room = 40;
    char* p = reserve(room);
    char* end = zmij::write(p, room, x);
    if (std::find_if(p, end, [](char c) { return c == '.' || c == 'e'; }) == end) {
      std::memcpy(end, ".0", 2);
      end += 2;
    }
    size_ = end - data_;
  }
  // Writes the string between quotes, escaped as JSON requires: quotes,
  // backslashes and control characters.
  void escape_and_append_with_quotes(std::string_view s) {
    // At most 6 bytes (\u00XX) per input byte.
    char* out = reserve(2 + 6 * s.size());
    *out++ = '"';
    const char* in = s.data();
    const char* const end = in + s.size();
    while (in != end) {
      // Copy the bytes that need no escaping, 8 at a time.
      while (end - in >= 8) {
        uint64_t word;
        std::memcpy(&word, in, 8);
        if (needs_escape(word)) break;
        std::memcpy(out, in, 8);
        in += 8;
        out += 8;
      }
      while (in != end && !needs_escape(static_cast<unsigned char>(*in))) *out++ = *in++;
      if (in == end) break;
      out = escape(out, static_cast<unsigned char>(*in++));
    }
    *out++ = '"';
    size_ = out - data_;
  }
  void truncate(size_t size) { size_ = size; }
  // Counts as written the n bytes after the end (which reserve gave).
  void advance(size_t n) { size_ += n; }
  // Gives up the buffer (with a terminating zero after the size() bytes).
  char* release(size_t* size) {
    *reserve(1) = '\0';
    char* p = data_;
    *size = size_;
    data_ = nullptr;
    size_ = capacity_ = 0;
    return p;
  }

 private:
  static bool needs_escape(unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }
  // Whether one of the 8 bytes is a control character, a quote or a backslash.
  static bool needs_escape(uint64_t word) {
    static constexpr uint64_t ones = 0x0101010101010101;  // static: MSVC wants it captured otherwise
    static constexpr uint64_t highs = 0x8080808080808080;
    auto has_zero = [](uint64_t v) { return (v - ones) & ~v & highs; };
    const uint64_t control = (word - 0x20 * ones) & ~word & highs;  // a byte < 0x20
    return (control | has_zero(word ^ ('"' * ones)) | has_zero(word ^ ('\\' * ones))) != 0;
  }
  static char* escape(char* out, unsigned char c) {
    *out++ = '\\';
    switch (c) {
      case '"':
        *out++ = '"';
        break;
      case '\\':
        *out++ = '\\';
        break;
      case '\b':
        *out++ = 'b';
        break;
      case '\f':
        *out++ = 'f';
        break;
      case '\n':
        *out++ = 'n';
        break;
      case '\r':
        *out++ = 'r';
        break;
      case '\t':
        *out++ = 't';
        break;
      default: {
        static const char hex[] = "0123456789abcdef";
        std::memcpy(out, "u00", 3);
        out[3] = hex[c >> 4];
        out[4] = hex[c & 15];
        out += 5;
      }
    }
    return out;
  }
  void grow(size_t n) {
    size_t capacity = capacity_ < 4096 ? 4096 : capacity_ * 2;
    if (capacity - size_ < n) capacity = size_ + n;
    char* p = allocator_ == nullptr
                  ? static_cast<char*>(std::realloc(data_, capacity))
                  : allocator_->reallocate(allocator_->context, data_, size_, capacity);
    if (p == nullptr) throw std::bad_alloc();
    data_ = p;
    capacity_ = capacity;
  }
  const ndjson_allocator* allocator_{nullptr};
  char* data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

enum class kind {
  null,
  boolean,
  int8,
  uint8,
  int16,
  uint16,
  int32,
  uint32,
  int64,
  uint64,
  float32,
  float64,
  utf8,        // int32 offsets
  large_utf8,  // int64 offsets
  utf8_view,
  struct_,
  list,        // int32 offsets
  large_list,  // int64 offsets
  fixed_list,
};

// What we need to write the values of one Arrow array (and its children).
struct node {
  kind k{kind::null};
  int64_t length{0};
  int64_t offset{0};
  const uint8_t* validity{nullptr};  // null when there are no nulls
  const void* values{nullptr};       // values, offsets or views
  const char* chars{nullptr};        // bytes of utf8 and large_utf8
  const void* const* view_buffers{nullptr};  // data buffers of utf8_view
  int64_t n_view_buffers{0};
  int64_t fixed_size{0};
  std::vector<node> children;
  std::vector<std::string> keys;  // for a struct: "name": (escaped, quoted)

  bool is_null(int64_t i) const {
    if (validity == nullptr) return false;
    const int64_t j = offset + i;
    return ((validity[j >> 3] >> (j & 7)) & 1) == 0;
  }
};

bool get_bit(const void* bits, int64_t j) {
  return (static_cast<const uint8_t*>(bits)[j >> 3] >> (j & 7)) & 1;
}

status make_node(const ArrowSchema* schema, const ArrowArray* array, node* out, int depth) {
  if (depth > 1024) return status{"Arrow data too deeply nested"};
  if (schema == nullptr || array == nullptr || schema->format == nullptr) {
    return status{"invalid Arrow data"};
  }
  if (schema->dictionary != nullptr) return status{"dictionary arrays are not supported yet"};
  const std::string_view format(schema->format);
  const std::string name = schema->name != nullptr ? schema->name : "";
  node& n = *out;
  n.length = array->length;
  n.offset = array->offset;
  int64_t expected_buffers = 2;
  int64_t expected_children = 0;
  if (format == "n") {
    n.k = kind::null;
    expected_buffers = 0;
  } else if (format == "b") {
    n.k = kind::boolean;
  } else if (format == "c") {
    n.k = kind::int8;
  } else if (format == "C") {
    n.k = kind::uint8;
  } else if (format == "s") {
    n.k = kind::int16;
  } else if (format == "S") {
    n.k = kind::uint16;
  } else if (format == "i") {
    n.k = kind::int32;
  } else if (format == "I") {
    n.k = kind::uint32;
  } else if (format == "l") {
    n.k = kind::int64;
  } else if (format == "L") {
    n.k = kind::uint64;
  } else if (format == "f") {
    n.k = kind::float32;
  } else if (format == "g") {
    n.k = kind::float64;
  } else if (format == "u") {
    n.k = kind::utf8;
    expected_buffers = 3;
  } else if (format == "U") {
    n.k = kind::large_utf8;
    expected_buffers = 3;
  } else if (format == "vu") {
    n.k = kind::utf8_view;
    expected_buffers = -1;  // variadic
  } else if (format == "+s") {
    n.k = kind::struct_;
    expected_buffers = 1;
    expected_children = schema->n_children;
  } else if (format == "+l") {
    n.k = kind::list;
    expected_children = 1;
  } else if (format == "+L") {
    n.k = kind::large_list;
    expected_children = 1;
  } else if (format.size() > 3 && format.substr(0, 3) == "+w:") {
    n.k = kind::fixed_list;
    expected_buffers = 1;
    expected_children = 1;
    n.fixed_size = std::atoll(std::string(format.substr(3)).c_str());
    if (n.fixed_size < 0) return status{"invalid fixed-size list"};
  } else {
    return status{"unsupported Arrow type (format \"" + std::string(format) + "\") for field " +
                  name};
  }
  if ((expected_buffers >= 0 && array->n_buffers != expected_buffers) ||
      (expected_buffers < 0 && array->n_buffers < 3) || schema->n_children != expected_children ||
      array->n_children != expected_children) {
    return status{"invalid Arrow array for field " + name};
  }
  if (n.k != kind::null && array->null_count != 0) {
    n.validity = static_cast<const uint8_t*>(array->buffers[0]);
  }
  switch (n.k) {
    case kind::utf8:
    case kind::large_utf8:
      n.values = array->buffers[1];
      n.chars = static_cast<const char*>(array->buffers[2]);
      break;
    case kind::utf8_view:
      n.values = array->buffers[1];
      n.view_buffers = array->buffers + 2;
      n.n_view_buffers = array->n_buffers - 3;  // the last buffer holds the sizes
      break;
    case kind::null:
    case kind::struct_:
    case kind::fixed_list:
      break;
    default:
      n.values = array->buffers[1];
  }
  n.children.resize(static_cast<size_t>(expected_children));
  for (int64_t i = 0; i < expected_children; i++) {
    status s = make_node(schema->children[i], array->children[i], &n.children[i], depth + 1);
    if (!s.ok()) return s;
  }
  if (n.k == kind::struct_) {
    for (int64_t i = 0; i < expected_children; i++) {
      const char* key = schema->children[i]->name;
      output o;
      o.escape_and_append_with_quotes(key != nullptr ? key : "");
      o.append_colon();
      n.keys.emplace_back(o.view());
    }
  }
  return status{};
}

class value_writer {
 public:
  value_writer(output& b, const ndjson_write_options& options) : b_(b), options_(options) {}

  void write_value(const node& n, int64_t i) {
    if (n.k == kind::null || n.is_null(i)) {
      b_.append_null();
      return;
    }
    const int64_t j = n.offset + i;
    switch (n.k) {
      case kind::null:
        break;
      case kind::boolean:
        b_.append_bool(get_bit(n.values, j));
        break;
      case kind::int8:
        b_.append_int(static_cast<const int8_t*>(n.values)[j]);
        break;
      case kind::uint8:
        b_.append_uint(static_cast<const uint8_t*>(n.values)[j]);
        break;
      case kind::int16:
        b_.append_int(static_cast<const int16_t*>(n.values)[j]);
        break;
      case kind::uint16:
        b_.append_uint(static_cast<const uint16_t*>(n.values)[j]);
        break;
      case kind::int32:
        b_.append_int(static_cast<const int32_t*>(n.values)[j]);
        break;
      case kind::uint32:
        b_.append_uint(static_cast<const uint32_t*>(n.values)[j]);
        break;
      case kind::int64:
        b_.append_int(static_cast<const int64_t*>(n.values)[j]);
        break;
      case kind::uint64:
        b_.append_uint(static_cast<const uint64_t*>(n.values)[j]);
        break;
      case kind::float32: {
        const float x = static_cast<const float*>(n.values)[j];
        if (std::isfinite(x)) {
          b_.append_float(x);
        } else {
          b_.append_null();  // JSON has no NaN or infinity
        }
        break;
      }
      case kind::float64: {
        const double x = static_cast<const double*>(n.values)[j];
        if (std::isfinite(x)) {
          b_.append_float(x);
        } else {
          b_.append_null();
        }
        break;
      }
      case kind::utf8: {
        const int32_t* offsets = static_cast<const int32_t*>(n.values);
        b_.escape_and_append_with_quotes(
            std::string_view(n.chars + offsets[j], static_cast<size_t>(offsets[j + 1] - offsets[j])));
        break;
      }
      case kind::large_utf8: {
        const int64_t* offsets = static_cast<const int64_t*>(n.values);
        b_.escape_and_append_with_quotes(
            std::string_view(n.chars + offsets[j], static_cast<size_t>(offsets[j + 1] - offsets[j])));
        break;
      }
      case kind::utf8_view: {
        // 16 bytes per value: the length, then the bytes when there are at most
        // 12 of them, otherwise a prefix, the index of a buffer and an offset.
        const uint8_t* view = static_cast<const uint8_t*>(n.values) + 16 * j;
        int32_t length;
        std::memcpy(&length, view, 4);
        if (length <= 12) {
          b_.escape_and_append_with_quotes(
              std::string_view(reinterpret_cast<const char*>(view + 4), static_cast<size_t>(length)));
        } else {
          int32_t index, offset;
          std::memcpy(&index, view + 8, 4);
          std::memcpy(&offset, view + 12, 4);
          const char* data = static_cast<const char*>(n.view_buffers[index]);
          b_.escape_and_append_with_quotes(
              std::string_view(data + offset, static_cast<size_t>(length)));
        }
        break;
      }
      case kind::struct_:
        write_object(n, i);
        break;
      case kind::list: {
        const int32_t* offsets = static_cast<const int32_t*>(n.values);
        write_array(n.children[0], offsets[j], offsets[j + 1]);
        break;
      }
      case kind::large_list: {
        const int64_t* offsets = static_cast<const int64_t*>(n.values);
        write_array(n.children[0], offsets[j], offsets[j + 1]);
        break;
      }
      case kind::fixed_list:
        write_array(n.children[0], j * n.fixed_size, (j + 1) * n.fixed_size);
        break;
    }
  }

  // Writes the struct value at index i (not null) as a JSON object.
  void write_object(const node& n, int64_t i) {
    // The children of a struct are at the index of the struct, offset included.
    const int64_t j = n.offset + i;
    b_.append('{');
    bool first = true;
    for (size_t c = 0; c < n.children.size(); c++) {
      const node& child = n.children[c];
      if (!options_.write_nulls && (child.k == kind::null || child.is_null(j))) continue;
      if (!first) b_.append_comma();
      first = false;
      b_.append_raw(n.keys[c]);
      write_value(child, j);
    }
    b_.append('}');
  }

  void write_array(const node& item, int64_t begin, int64_t end) {
    b_.append('[');
    for (int64_t i = begin; i < end; i++) {
      if (i != begin) b_.append_comma();
      write_value(item, i);
    }
    b_.append(']');
  }

 private:
  output& b_;
  const ndjson_write_options& options_;
};

status make_root(const ArrowSchema* schema, const ArrowArray* batch, node* root) {
  status s = make_node(schema, batch, root, 0);
  if (s.ok() && root->k != kind::struct_) s = status{"a record batch must be a struct array"};
  return s;
}

// Writes the rows [begin, end) of a record batch.
void write_rows(const node& root, int64_t begin, int64_t end, output& b,
                const ndjson_write_options& options) {
  value_writer writer(b, options);
  for (int64_t i = begin; i < end; i++) {
    if (root.is_null(i)) {
      b.append_null();
    } else {
      writer.write_object(root, i);
    }
    b.append('\n');
  }
}

// Writes the batches with threads: each thread writes ranges of rows into
// buffers of its own, which we then copy, in order, after the end of out.
void write_in_parallel(const std::vector<node>& roots, output& out,
                       const ndjson_write_options& options) {
  struct range {
    size_t batch;
    int64_t begin, end;
  };
  int64_t total = 0;
  for (const node& root : roots) total += root.length;
  // Enough ranges for the threads to share the work, not so many that they cost.
  const int64_t rows = std::max<int64_t>(
      1024, std::min<int64_t>(65536, total / static_cast<int64_t>(options.threads * 8) + 1));
  std::vector<range> ranges;
  for (size_t b = 0; b < roots.size(); b++) {
    for (int64_t i = 0; i < roots[b].length; i += rows) {
      ranges.push_back(range{b, i, std::min(roots[b].length, i + rows)});
    }
  }
  // Each buffer on its own cache line: threads update them all the time.
  struct alignas(64) part {
    output o;
  };
  std::vector<part> parts(ranges.size());
  internal::parallel_for(ranges.size(), options.threads, [&](size_t r) {
    write_rows(roots[ranges[r].batch], ranges[r].begin, ranges[r].end, parts[r].o, options);
  });
  std::vector<size_t> where(parts.size());
  size_t size = 0;
  for (size_t r = 0; r < parts.size(); r++) {
    where[r] = size;
    size += parts[r].o.size();
  }
  char* base = out.reserve(size + 1);  // with room for the zero of release
  internal::parallel_for(parts.size(), options.threads, [&](size_t r) {
    const std::string_view part = parts[r].o.view();
    if (!part.empty()) std::memcpy(base + where[r], part.data(), part.size());
  });
  out.advance(size);
}

}  // namespace

struct ndjson_writer::impl {
  impl(const ndjson_write_options& o, const ndjson_allocator* a)
      : options(o), allocator(a ? *a : ndjson_allocator{}), out(a ? &allocator : nullptr) {}
  ndjson_write_options options;
  ndjson_allocator allocator;
  output out;
};

ndjson_writer::ndjson_writer(const ndjson_write_options& options)
    : impl_(new impl(options, nullptr)) {}

ndjson_writer::ndjson_writer(const ndjson_write_options& options,
                             const ndjson_allocator& allocator)
    : impl_(new impl(options, &allocator)) {}

ndjson_writer::~ndjson_writer() = default;

status ndjson_writer::write(const ArrowSchema* schema, const ArrowArray* batch) {
  return write(schema, &batch, 1);
}

status ndjson_writer::write(const ArrowSchema* schema, const ArrowArray* const* batches,
                            size_t n) {
  const size_t size = impl_->out.size();
  try {
    std::vector<node> roots(n);
    for (size_t b = 0; b < n; b++) {
      status s = make_root(schema, batches[b], &roots[b]);
      if (!s.ok()) return s;
    }
    if (impl_->options.threads > 1) {
      write_in_parallel(roots, impl_->out, impl_->options);
    } else {
      for (const node& root : roots) write_rows(root, 0, root.length, impl_->out, impl_->options);
    }
    return status{};
  } catch (const std::bad_alloc&) {
    impl_->out.truncate(size);
    return status{"out of memory"};
  } catch (const std::exception& e) {
    impl_->out.truncate(size);
    return status{e.what()};
  }
}

status ndjson_writer::write(ArrowArrayStream* stream) {
  ArrowSchema schema;
  if (stream->get_schema(stream, &schema) != 0) {
    const char* e = stream->get_last_error(stream);
    stream->release(stream);
    return status{std::string("cannot get the schema of the stream: ") + (e ? e : "")};
  }
  // We take all the batches, so that threads can share them.
  std::vector<ArrowArray> batches;
  status s;
  while (true) {
    ArrowArray batch;
    if (stream->get_next(stream, &batch) != 0) {
      const char* e = stream->get_last_error(stream);
      s = status{std::string("cannot read the stream: ") + (e ? e : "")};
      break;
    }
    if (batch.release == nullptr) break;  // end of the stream
    try {
      batches.push_back(batch);
    } catch (const std::bad_alloc&) {
      batch.release(&batch);
      s = status{"out of memory"};
      break;
    }
  }
  if (s.ok()) {
    try {
      std::vector<const ArrowArray*> pointers;
      for (const ArrowArray& batch : batches) pointers.push_back(&batch);
      s = write(&schema, pointers.data(), pointers.size());
    } catch (const std::bad_alloc&) {
      s = status{"out of memory"};
    }
  }
  for (ArrowArray& batch : batches) batch.release(&batch);
  schema.release(&schema);
  stream->release(stream);
  return s;
}

std::string_view ndjson_writer::view() const { return impl_->out.view(); }

char* ndjson_writer::release(size_t* size) { return impl_->out.release(size); }

status write_ndjson(const ArrowSchema* schema, const ArrowArray* batch, std::string* out,
                    const ndjson_write_options& options) {
  ndjson_writer writer(options);
  status s = writer.write(schema, batch);
  if (s.ok()) out->append(writer.view());
  return s;
}

status write_ndjson(ArrowArrayStream* stream, std::string* out,
                    const ndjson_write_options& options) {
  ndjson_writer writer(options);
  status s = writer.write(stream);
  if (s.ok()) out->append(writer.view());
  return s;
}

}  // namespace simdjson_arrow

extern "C" int simdjson_arrow_write_ndjson(struct ArrowArrayStream* stream, int write_nulls,
                                           size_t threads, char** out, size_t* out_length,
                                           char* error, size_t error_capacity) {
  using namespace simdjson_arrow;
  status s;
  try {
    ndjson_write_options options;
    options.write_nulls = write_nulls != 0;
    options.threads = threads == 0 ? 1 : threads;
    ndjson_writer writer(options);
    s = writer.write(stream);
    if (s.ok()) {
      *out = writer.release(out_length);
      return 0;
    }
  } catch (const std::bad_alloc&) {
    s = status{"out of memory"};
  } catch (const std::exception& e) {
    s = status{e.what()};
  } catch (...) {
    s = status{"unknown error"};
  }
  if (error != nullptr && error_capacity > 0) {
    size_t n = s.message.size() < error_capacity - 1 ? s.message.size() : error_capacity - 1;
    std::memcpy(error, s.message.data(), n);
    error[n] = '\0';
  }
  return 1;
}

extern "C" void simdjson_arrow_free(void* p) { std::free(p); }

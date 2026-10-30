// simdjson_arrow: newline-delimited JSON to Apache Arrow, with simdjson.
//
// The results are Arrow C Data Interface structures (ArrowSchema, ArrowArray,
// ArrowArrayStream): any Arrow implementation (Arrow C++, pyarrow, polars,
// DuckDB, nanoarrow...) can take them without copying. We do not depend on
// Arrow.
#ifndef SIMDJSON_ARROW_H
#define SIMDJSON_ARROW_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "simdjson.h"

// The Arrow C Data Interface and C Stream Interface, as specified in
// https://arrow.apache.org/docs/format/CDataInterface.html
// The guards let this header coexist with Arrow's own arrow/c/abi.h.
extern "C" {
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct ArrowSchema** children;
  struct ArrowSchema* dictionary;
  void (*release)(struct ArrowSchema*);
  void* private_data;
};

struct ArrowArray {
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct ArrowArray** children;
  struct ArrowArray* dictionary;
  void (*release)(struct ArrowArray*);
  void* private_data;
};
#endif  // ARROW_C_DATA_INTERFACE

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE
struct ArrowArrayStream {
  int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);
  int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);
  const char* (*get_last_error)(struct ArrowArrayStream*);
  void (*release)(struct ArrowArrayStream*);
  void* private_data;
};
#endif  // ARROW_C_STREAM_INTERFACE
}

namespace simdjson_arrow {

// The Arrow types we read and write.
enum class type {
  null,     // "n"
  boolean,  // "b"
  int64,    // "l"
  uint64,   // "L"
  float64,  // "g"
  utf8,     // "u"
  struct_,  // "+s"
  list,     // "+l"
};

// A field of a schema. The schema of a table is a struct field whose children
// are the columns.
struct field {
  std::string name;
  simdjson_arrow::type type{simdjson_arrow::type::null};
  bool nullable{true};
  std::vector<field> children;  // the fields of a struct, the item of a list
};

inline field make_field(std::string name, type t, bool nullable = true) {
  return field{std::move(name), t, nullable, {}};
}
inline field make_struct(std::string name, std::vector<field> children, bool nullable = true) {
  return field{std::move(name), type::struct_, nullable, std::move(children)};
}
inline field make_list(std::string name, field item, bool nullable = true) {
  return field{std::move(name), type::list, nullable, {std::move(item)}};
}
inline field make_schema(std::vector<field> columns) {
  return field{"", type::struct_, false, std::move(columns)};
}

// The result of an operation: empty when it succeeded, otherwise an error message.
struct status {
  std::string message;
  bool ok() const { return message.empty(); }
};

// Converts between our fields and Arrow schemas. import_schema does not take
// ownership of the ArrowSchema; export_schema fills out, which the caller
// must release.
status import_schema(const ArrowSchema* schema, field* out);
status export_schema(const field& schema, ArrowSchema* out);

struct ndjson_options {
  // We start a new record batch once a batch holds this many bytes of JSON.
  // No record may be larger than block_size.
  size_t block_size{1 << 20};
  // A key absent from the schema is an error (otherwise it is skipped).
  bool error_on_unknown_fields{false};
  // Check that skipped values are valid JSON. Otherwise only their structure
  // is checked.
  bool validate_skipped{true};
};

// Reads newline-delimited JSON (one object per record, whitespace between
// records) into record batches, given the schema of the records. Each batch
// is a struct ArrowArray, appended to batches; the caller must release them.
// A JSON key absent from the schema is skipped (or an error), a field absent
// from the record is null.
status read_ndjson(const field& schema, simdjson::padded_string_view json,
                   std::vector<ArrowArray>* batches, const ndjson_options& options = {});

// Makes an ArrowArrayStream out of a schema and record batches, which the
// stream takes over.
void make_stream(const field& schema, std::vector<ArrowArray>&& batches, ArrowArrayStream* out);

}  // namespace simdjson_arrow

extern "C" {
// C entry point (for bindings): reads the newline-delimited JSON in
// data[0, length) into out. When capacity >= length + SIMDJSON_PADDING, the
// input is read in place, otherwise it is copied. The schema is not released.
// Returns 0 on success; otherwise writes a message in error[0, error_capacity)
// and returns a non-zero value.
int simdjson_arrow_read_ndjson(const char* data, size_t length, size_t capacity,
                               const struct ArrowSchema* schema, size_t block_size,
                               int error_on_unknown_fields, int validate_skipped,
                               struct ArrowArrayStream* out, char* error,
                               size_t error_capacity);
}

#endif  // SIMDJSON_ARROW_H

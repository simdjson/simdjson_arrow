# simdjson_arrow

JSON <=> Apache Arrow, using simdjson.

simdjson_arrow reads newline-delimited JSON (NDJSON, JSON Lines) into
[Apache Arrow](https://arrow.apache.org/) record batches, with
[simdjson](https://github.com/simdjson/simdjson), and writes Arrow record
batches as newline-delimited JSON. It is several times faster than Arrow C++'s
own JSON reader (see [Performance](#performance)), on one thread or many.

- Read with a schema, or infer it.
- Read and write on many threads.
- Results are [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
  structures (`ArrowSchema`, `ArrowArray`, `ArrowArrayStream`): Arrow C++,
  nanoarrow, DuckDB or any other Arrow implementation takes them without a
  copy.
- No dependency on Arrow: only on simdjson (and on
  [zmij](https://github.com/vitaut/zmij), included, to write floating-point
  numbers).
- C++17, one header and one source file. Tested with GCC, clang and MSVC, on
  Linux, macOS and Windows.

Status: early (0.1). The interface may change.

## Contents

- [Adding it to your project](#adding-it-to-your-project)
- [Quick start](#quick-start)
- [Reading JSON](#reading-json)
- [Writing JSON](#writing-json)
- [With Arrow C++](#with-arrow-c)
- [The C interface](#the-c-interface)
- [Errors, memory and threads](#errors-memory-and-threads)
- [Limits](#limits)
- [Building and testing](#building-and-testing)
- [Performance](#performance)
- [License](#license)

## Adding it to your project

The simplest way: take two files from a
[release](https://github.com/simdjson/simdjson_arrow/releases), `simdjson_arrow.h`
and `simdjson_arrow.cpp` (or `singleheader.zip`, which holds both), and the two
files of a [simdjson release](https://github.com/simdjson/simdjson/releases)
(5.0.7 or later), `simdjson.h` and `simdjson.cpp`. Put the four files in one
directory, include `simdjson_arrow.h`, and compile the two source files with
yours, as C++17:

```sh
c++ -O3 -std=c++17 myprogram.cpp simdjson_arrow.cpp simdjson.cpp -o myprogram
```

With MSVC: `cl /std:c++17 /O2 /EHsc myprogram.cpp simdjson_arrow.cpp simdjson.cpp`.
Add `-pthread` if your toolchain needs it for `std::thread` (older Linux
systems). If you already use simdjson, keep your copy: `simdjson_arrow.h`
includes `"simdjson.h"`, which must be on the include path.

With CMake, the `simdjson_arrow` target brings simdjson with it:

```cmake
include(FetchContent)
FetchContent_Declare(simdjson_arrow
  GIT_REPOSITORY https://github.com/simdjson/simdjson_arrow.git
  GIT_TAG v0.1.0)
set(SIMDJSON_ARROW_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ARROW_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ARROW_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson_arrow)
target_link_libraries(myprogram PRIVATE simdjson_arrow)
```

From a checkout, `python3 tools/amalgamate.py` writes the two single files in
`singleheader/`.

## Quick start

```cpp
#include "simdjson_arrow.h"
using namespace simdjson_arrow;

simdjson::padded_string json = simdjson::padded_string::load("events.ndjson");

// Read, with a schema.
field schema = make_schema({
    make_field("id", type::utf8),
    make_struct("actor", {make_field("id", type::int64), make_field("login", type::utf8)}),
    make_list("tags", make_field("item", type::utf8)),
});
std::vector<ArrowArray> batches;  // one struct array per record batch
status s = read_ndjson(schema, json, &batches);
if (!s.ok()) { /* s.message: "expected a string, found a number (row 7, field /id)" */ }

// Write back.
ArrowSchema arrow_schema;
export_schema(schema, &arrow_schema);
ndjson_writer writer;
for (const ArrowArray& b : batches) writer.write(&arrow_schema, &b);
std::string_view out = writer.view();

// Release what Arrow gave us.
arrow_schema.release(&arrow_schema);
for (ArrowArray& b : batches) b.release(&b);
```

[`examples/example.cpp`](examples/example.cpp) is a complete program;
[`examples/arrow_cpp.cpp`](examples/arrow_cpp.cpp) goes through Arrow C++.

## Reading JSON

```cpp
// Given a schema.
status read_ndjson(const field& schema, simdjson::padded_string_view json,
                   std::vector<ArrowArray>* batches, const ndjson_options& options = {});
// Inferring the schema, written in *schema.
status read_ndjson(simdjson::padded_string_view json, field* schema,
                   std::vector<ArrowArray>* batches, const ndjson_options& options = {});
// The same, for input without simdjson's padding.
status read_ndjson(const field& schema, const char* json, size_t length,
                   std::vector<ArrowArray>* batches, const ndjson_options& options = {});
status read_ndjson(const char* json, size_t length, field* schema,
                   std::vector<ArrowArray>* batches, const ndjson_options& options = {});
```

The input holds JSON objects, one per record, separated by whitespace (usually
a line end). We append the batches to `batches`, each a struct `ArrowArray`
whose children are the columns: the caller releases them (`b.release(&b)`).
`make_stream(schema, std::move(batches), &stream)` turns them into an
`ArrowArrayStream`, which takes them over.

### Schemas

A schema is a `field` of type `struct_` whose children are the columns; build
one with `make_schema`, `make_field`, `make_struct` and `make_list`, or from an
Arrow schema with `import_schema(const ArrowSchema*, field*)`. `export_schema`
goes the other way.

| our type | Arrow type | JSON values |
|---|---|---|
| `null` | null (`n`) | `null` |
| `boolean` | bool (`b`) | `true`, `false` |
| `int64` | int64 (`l`) | integers from -2^63 to 2^63-1 |
| `uint64` | uint64 (`L`) | integers from 0 to 2^64-1 |
| `float64` | float64 (`g`) | any number |
| `utf8` | utf8 (`u`) | strings (unescaped) |
| `struct_` | struct (`+s`) | objects |
| `list` | list (`+l`) | arrays |

The rules, as with Arrow's JSON reader:

- a field absent from a record is null (an error if the field is not nullable);
- a key absent from the schema is skipped (or an error, with
  `error_on_unknown_fields`);
- a value of another type is an error: we do not convert between types,
  except that a `float64` field accepts any JSON number;
- a duplicate key is an error.

Skipped values are checked to be valid JSON, unless `validate_skipped` is false
(then simdjson only checks their structure, which is faster).

### Inferring the schema

```cpp
field schema;  // we fill it
status s = read_ndjson(json, &schema, &batches);
```

As Arrow's JSON reader does: `null` is of type null, `true` and `false` are
booleans, integers that fit in 64 bits are int64, other numbers are float64,
strings are utf8, objects are structs and arrays are lists. A field of nulls
takes the type of its other values, int64 becomes float64 next to other
numbers, and the fields of objects are united, in the order in which they
first appear. Other mixes (a string and a number, say) are an error. Unlike
Arrow, we do not infer timestamps: strings stay strings. We infer in one pass:
each batch gets its own schema, and we merge the schemas at the end,
converting the batches that need it. All the batches have the final schema.

### Options

```cpp
struct ndjson_options {
  size_t block_size{1 << 20};           // bytes of JSON per record batch
  size_t threads{1};                    // threads that read
  bool error_on_unknown_fields{false};  // a key absent from the schema is an error
  bool validate_skipped{true};          // check that skipped values are valid JSON
};
```

With `threads` above one, threads read blocks of about `block_size` bytes (one
block, one batch). Each thread takes the next block from a shared counter and
moves both of its ends forward to the next line start, so that the blocks are
adjacent and hold whole lines. A record must then fit on one line, as with
Arrow's reader by default. If reading in blocks fails, we read the input again
in one thread, so that records may span lines and errors are the same (same
message, same row) whatever the number of threads. Smaller blocks mean more,
smaller batches, which threads share more evenly.

### Input without padding

simdjson wants `SIMDJSON_PADDING` bytes (64) after the input. When you cannot
give them (a memory-mapped file, a buffer that another library owns), pass a
pointer and a length: we read the input in place, in blocks cut at line ends,
and copy only the blocks that end near its end. A `simdjson::padded_string`
(from `padded_string::load`) has the padding.

## Writing JSON

```cpp
class ndjson_writer {
 public:
  explicit ndjson_writer(const ndjson_write_options& options = {});
  ndjson_writer(const ndjson_write_options& options, const ndjson_allocator& allocator);
  status write(const ArrowSchema* schema, const ArrowArray* batch);  // one batch
  status write(const ArrowSchema* schema, const ArrowArray* const* batches, size_t n);
  status write(ArrowArrayStream* stream);  // all the batches; releases the stream
  std::string_view view() const;           // the JSON so far
  char* release(size_t* size);             // gives up the buffer (free it with free)
};
// Shortcuts that append to a std::string.
status write_ndjson(const ArrowSchema* schema, const ArrowArray* batch, std::string* out,
                    const ndjson_write_options& options = {});
status write_ndjson(ArrowArrayStream* stream, std::string* out,
                    const ndjson_write_options& options = {});
```

We write one JSON object per row, one row per line. The batch is a struct
array (a record batch), not released by `write`; successive calls append to
the output, and a call that fails writes nothing.

```cpp
struct ndjson_write_options {
  bool write_nulls{true};  // a null field is written as "key":null, otherwise omitted
  size_t threads{1};       // threads write ranges of rows into buffers that we then join
};
```

We write the Arrow types null, bool, the integers (8 to 64 bits, signed or
not), float32, float64, utf8, large utf8, utf8 view, struct, list, large list
and fixed-size list, with any offsets (slices) and null bitmaps.
Floating-point numbers get the shortest representation that gives them back,
as Python's `repr` (`1.0` rather than `1`, `1e+21`); NaN and infinities, which
JSON lacks, become `null`. Strings are escaped as JSON requires (quotes,
backslashes and control characters); other characters are written as they are
(UTF-8).

By default the writer grows a buffer from `malloc`. To write into memory of
your own (say, a buffer of another language, handed over without a copy),
give it an `ndjson_allocator`; `release` then gives up a buffer from your
`reallocate`:

```cpp
struct ndjson_allocator {
  // A buffer of at least size bytes beginning with the used bytes of p (null at
  // first), or null if it cannot.
  char* (*reallocate)(void* context, char* p, size_t used, size_t size);
  void (*deallocate)(void* context, char* p);  // frees a buffer not given up
  void* context;
};
```

## With Arrow C++

Arrow C++ imports our batches and exports its own through `arrow/c/bridge.h`,
without a copy. Our header defines the C Data Interface structures with the
same guards as Arrow's `arrow/c/abi.h`, so that both headers can be included.

```cpp
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "simdjson_arrow.h"

// NDJSON to an arrow::Table.
simdjson_arrow::field schema;
std::vector<ArrowArray> batches;
simdjson_arrow::status s = simdjson_arrow::read_ndjson(json, &schema, &batches);
ArrowArrayStream stream;
simdjson_arrow::make_stream(schema, std::move(batches), &stream);
std::shared_ptr<arrow::Table> table =
    arrow::ImportRecordBatchReader(&stream).ValueOrDie()->ToTable().ValueOrDie();

// An arrow::Table to NDJSON.
ArrowArrayStream out;
arrow::ExportRecordBatchReader(std::make_shared<arrow::TableBatchReader>(*table), &out);
std::string ndjson;
s = simdjson_arrow::write_ndjson(&out, &ndjson);
```

[`examples/arrow_cpp.cpp`](examples/arrow_cpp.cpp) does this with error
handling; build it with `-DSIMDJSON_ARROW_ARROW_CPP=ON` (see
[Building and testing](#building-and-testing)).

## The C interface

Two functions with C linkage do it all, for programs or languages that cannot
call C++:

```c
// Reads data[0, length) into an ArrowArrayStream. With capacity >= length +
// SIMDJSON_PADDING, the input is read in place, otherwise it is copied. A null
// schema means: infer it. Zero for block_size or threads means the default.
int simdjson_arrow_read_ndjson(const char* data, size_t length, size_t capacity,
                               const struct ArrowSchema* schema, size_t block_size,
                               int error_on_unknown_fields, int validate_skipped,
                               size_t threads, struct ArrowArrayStream* out,
                               char* error, size_t error_capacity);
// Writes the stream (which is released); *out is to be freed with simdjson_arrow_free.
int simdjson_arrow_write_ndjson(struct ArrowArrayStream* stream, int write_nulls,
                                size_t threads, char** out, size_t* out_length,
                                char* error, size_t error_capacity);
void simdjson_arrow_free(void* p);
```

They return 0 on success; otherwise a non-zero value, with the message in
`error`.

## Errors, memory and threads

- Reading and writing report errors with a `status` (`ok()`, `message`), not
  with exceptions, even when memory runs out ("out of memory"). On error,
  `read_ndjson` leaves `batches` as it was. The small helpers
  (`import_schema`, `export_schema`, `make_stream`) may throw `std::bad_alloc`.
- Reading errors give the row (counted from 0) and the field, e.g. `expected a
  64-bit signed integer, found a string (row 1, field /id)` or `duplicate key
  (row 3, field /actor/login)`.
- Each batch owns its buffers: release it when you are done, or hand it to
  Arrow, which will. The batches do not point into the input, which you may
  free once `read_ndjson` returns.
- The functions are safe to call from several threads at once; an
  `ndjson_writer` must be used by one thread at a time. The threads of
  `options.threads` are started for each call and joined before it returns.

## Limits

- One batch holds at most 2 GB of strings and 2^31 list items (Arrow's 32-bit
  offsets): lower `block_size` if a batch reaches that.
- With more than one thread, a record must fit on one line to be read in
  parallel (otherwise we read in one thread).
- JSON nested more than 1024 levels deep is an error.
- We do not read timestamps, dates, decimals, binary, dictionaries or maps
  (strings stay strings), and we do not write dictionaries, binary, decimals,
  timestamps or maps yet.

## Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build    # the C++ tests and the example
```

CMake downloads simdjson 5.0.7. Options: `SIMDJSON_ARROW_TESTS`,
`SIMDJSON_ARROW_BENCHMARKS` and `SIMDJSON_ARROW_EXAMPLES` (on by default), and
`SIMDJSON_ARROW_ARROW_CPP` (off), which builds `examples/arrow_cpp.cpp` and
`benchmarks/bench_arrow_cpp.cpp` against an installed Arrow C++ (with
`ARROW_JSON`):

```sh
cmake -B build-arrow -DSIMDJSON_ARROW_ARROW_CPP=ON -DCMAKE_PREFIX_PATH=/path/to/arrow
cmake --build build-arrow
```

Arrow C++'s own reader is also our reference: Python tests (they need pyarrow
and pytest, and load the library through its C interface) compare our tables
with those of Arrow's JSON reader, on random and edge-case inputs, and check
that writing and reading again gives back the same table:

```sh
python -m pytest tests
```

## Performance

Intel Xeon Gold 6548N (Emerald Rapids, two sockets of 32 cores), Linux,
GCC 14, best of five runs, in GB/s of JSON. Up to 32 threads, one thread per
core of one socket; 64 threads, one per core of both sockets.

- `flat`: 4 million flat records (integers, floats, short strings, booleans),
  611 MB, from `python3 benchmarks/gendata.py flat.ndjson`.
- `gh`: one hour of [GH Archive](https://www.gharchive.org/) events, 886 MB
  (`curl -sSfL https://data.gharchive.org/2024-03-13-12.json.gz | gunzip > gharchive.ndjson`).
  With a schema, we read six fields (`id`, `type`, `actor` with its `id` and
  `login`, `repo` with its `id` and `name`, `public`, `created_at`) and skip the
  rest; inferring the schema, we read everything (1,114 columns).

Reading, against the JSON reader of Arrow C++ (`arrow::json::TableReader`, with
the same schemas, ignoring the other fields, into a table): the 26.0.0 release,
which parses with RapidJSON, and the main branch of 2026-10-29 (a99464f9),
which parses with simdjson. Each cell: simdjson_arrow / Arrow 26.0.0 / Arrow main.

| threads | flat | gh | flat (inferred) | gh (inferred) |
|---|---|---|---|---|
| 1 | **0.71** / 0.17 / 0.21 | **1.95** / 0.47 / 0.77 | **0.72** / 0.18 / 0.20 | **0.34** / fails / fails |
| 8 | **5.5** / 1.4 / 1.6 | **14.9** / 3.7 / 6.0 | **5.5** / 1.4 / 1.6 | **1.4** / fails / fails |
| 32 | **17.8** / 5.3 / 6.0 | **40.7** / 13.0 / 18.5 | **16.9** / 5.3 / 5.7 | **2.3** / fails / fails |
| 64 | **24.6** / 7.8 / 8.3 | **35.7** / 17.0 / 19.9 | **21.5** / 7.3 / 7.4 | **2.3** / fails / fails |

Arrow cannot infer the schema of `gh`: it infers timestamps from some strings,
then fails to convert others ("JSON conversion to struct<... timestamp[s] ...>
is not supported"). Inferring the schema of `gh`, we build 1,114 columns
rather than six, which costs more.

Writing (Arrow C++ has no JSON writer), the same six fields of `gh` (60 MB):

| threads | flat | gh |
|---|---|---|
| 1 | 0.96 | 1.20 |
| 8 | 4.0 | 3.8 |
| 32 | 10.0 | 7.2 |
| 64 | 13.2 | 6.7 |

To run them:

```sh
./build/bench_ndjson flat.ndjson flat 5 1 32        # file, schema (flat, gh or infer), rounds, validate, threads
./build/bench_write flat.ndjson flat 5 32           # file, schema, rounds, threads
./build-arrow/bench_arrow_cpp flat.ndjson flat 5 1 32   # Arrow C++, same arguments as bench_ndjson
```

## License

Apache License 2.0. zmij (`third_party/zmij`) is under the MIT license.

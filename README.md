# simdjson_arrow

Reads newline-delimited JSON (NDJSON, JSON Lines) into [Apache Arrow](https://arrow.apache.org/)
record batches, with [simdjson](https://github.com/simdjson/simdjson), and writes
Arrow record batches as newline-delimited JSON.

The results are [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
structures (`ArrowSchema`, `ArrowArray`, `ArrowArrayStream`): Arrow C++, pyarrow,
polars, DuckDB or nanoarrow take them without copying. The library depends only on
simdjson (and on [zmij](https://github.com/vitaut/zmij), included, to write
floating-point numbers), not on Arrow. It is written in C++17.

Status: early. We read with a schema or infer it, on one or many threads, and
we write on one or many threads.

## Adding it to your project

The simplest way: take two files from a
[release](https://github.com/simdjson/simdjson_arrow/releases), `simdjson_arrow.h`
and `simdjson_arrow.cpp` (or `singleheader.zip`, which holds both), and the two
files of the matching [simdjson release](https://github.com/simdjson/simdjson/releases)
(5.0.7 or later), `simdjson.h` and `simdjson.cpp`. Put the four files in one
directory, include `simdjson_arrow.h`, and compile the two source files with
yours, as C++17:

```sh
c++ -O3 -std=c++17 myprogram.cpp simdjson_arrow.cpp simdjson.cpp -o myprogram
```

If you already use simdjson, keep your copy: `simdjson_arrow.h` includes
`"simdjson.h"`, so it must be on the include path. With MSVC: `cl /std:c++17 /O2
/EHsc myprogram.cpp simdjson_arrow.cpp simdjson.cpp`. Add `-pthread` (or the
equivalent) if your toolchain needs it for `std::thread`.

You can also write the two files from a checkout (`python3 tools/amalgamate.py`,
which puts them in `singleheader/`), or build with CMake (`add_subdirectory`
or `FetchContent`, then link with the `simdjson_arrow` target, which brings
simdjson with it).

## Usage

```cpp
#include "simdjson_arrow.h"
using namespace simdjson_arrow;

field schema = make_schema({
    make_field("id", type::utf8),
    make_struct("actor", {make_field("id", type::int64), make_field("login", type::utf8)}),
    make_list("tags", make_field("item", type::utf8)),
});
simdjson::padded_string json = simdjson::padded_string::load("events.ndjson");
std::vector<ArrowArray> batches;  // one struct array per record batch
status s = read_ndjson(schema, json, &batches);
if (!s.ok()) { /* s.message, e.g. "expected a string, found a number (row 7, field /id)" */ }
// ... hand the batches to Arrow, or release them:
for (ArrowArray& b : batches) b.release(&b);
```

A schema can also come from an `ArrowSchema` (`import_schema`), and
`make_stream` turns the batches into an `ArrowArrayStream`. The C function
`simdjson_arrow_read_ndjson` does it all, for bindings.

Types: `null`, `bool`, `int64`, `uint64`, `float64` (double), `utf8` (string),
`struct`, `list`. As with Arrow's JSON reader:

- a field absent from a record is null (an error if the field is not nullable);
- a key absent from the schema is skipped (or an error, with `error_on_unknown_fields`);
- a value of another type is an error: we do not convert between types, except
  that a `float64` field accepts any JSON number;
- duplicate keys are an error.

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
numbers, and the fields of objects are united, in the order in which they first
appear. Other mixes (a string and a number, say) are an error. We do not infer
timestamps: strings stay strings. We infer in one pass: each batch gets its own
schema, and we merge the schemas at the end, converting the batches that need it.

### Threads

With `options.threads` above one, threads read blocks of about `block_size`
bytes (one block, one batch). Each thread takes the next block from a shared
counter and moves both of its ends forward to the next line start, so that the
blocks are adjacent and hold whole lines. A record must then fit on one line, as
with Arrow's reader by default; if reading in blocks fails, we read the input
again in one thread, so that records may span lines and errors are the same
(same message, same row) whatever the number of threads.

### Input without padding

simdjson wants `SIMDJSON_PADDING` bytes after the input. When you cannot give
them (say, the bytes of a Python object), call `read_ndjson(schema, data, length,
&batches)` (or `read_ndjson(data, length, &schema, &batches)`): we read the input
in place, in blocks, and copy only the blocks that end near its end.

## Writing JSON

```cpp
ndjson_writer writer;  // or ndjson_writer writer(options), see ndjson_write_options
status s = writer.write(&schema, &batch);  // an ArrowSchema and a struct ArrowArray
// or writer.write(&stream) for all the batches of an ArrowArrayStream
std::string_view json = writer.view();  // or writer.release(&size): a buffer from malloc
```

We write one JSON object per row, one row per line, on `options.threads`
threads (each thread writes ranges of rows into its own buffer, and we copy the
buffers in order). We accept the Arrow types
null, bool, the integers (8 to 64 bits, signed or not), float32, float64,
utf8, large utf8, utf8 view, struct, list, large list and fixed-size list, with
any offsets. Floating-point numbers are written as Python's `repr` writes them
(the shortest representation that gives back the number, `1.0` rather than
`1`); NaN and infinities, which JSON lacks, become `null`. A null field is
written as `"key":null`, or omitted (`write_nulls = false`). The C function
`simdjson_arrow_write_ndjson` writes an `ArrowArrayStream`, for bindings.

A binding can have the writer grow a buffer of its own, and hand it over
without a copy: `ndjson_writer writer(options, allocator)`, where an
`ndjson_allocator` has a `reallocate` and a `deallocate` function. Then
`writer.release(&size)` gives up a buffer from `reallocate`. fastsimdjson
writes into a Python `bytes` object this way.

## Building

```sh
cmake -B build
cmake --build build
./build/basic_tests
```

CMake downloads simdjson 5.0.7. The Python tests compare our tables with those
of `pyarrow.json.read_json` (they need pyarrow and pytest):

```sh
python -m pytest tests
```

## Performance

Intel Xeon Gold 6548N (Emerald Rapids, two sockets of 32 cores), GCC 14, best
of several runs, in GB/s of JSON. Up to 32 threads, one thread per core of one
socket; 64 threads, one per core of both. `flat`: 4 million flat records
(numbers, strings, booleans), 611 MB. `gh`: one hour of GH Archive events,
886 MB; with a schema, we read six fields and skip the rest (and we write these
six fields, 60 MB); inferring the schema, we read everything (1,114 columns).

Reading, in C++, against the JSON reader of Arrow C++ (`arrow::json::TableReader`,
with the same schemas, into a table): the 26.0.0 release, which parses with
RapidJSON, and the main branch of 2026-10-29 (a99464f9), which parses with
simdjson. Each cell: simdjson_arrow / Arrow 26.0.0 / Arrow main.

| threads | flat | gh | flat (inferred) | gh (inferred) |
|---|---|---|---|---|
| 1 | **0.71** / 0.17 / 0.21 | **1.95** / 0.47 / 0.77 | **0.72** / 0.18 / 0.20 | **0.34** / fails / fails |
| 8 | **5.5** / 1.4 / 1.6 | **14.9** / 3.7 / 6.0 | **5.5** / 1.4 / 1.6 | **1.4** / fails / fails |
| 32 | **17.8** / 5.3 / 6.0 | **40.7** / 13.0 / 18.5 | **16.9** / 5.3 / 5.7 | **2.3** / fails / fails |
| 64 | **24.6** / 7.8 / 8.3 | **35.7** / 17.0 / 19.9 | **21.5** / 7.3 / 7.4 | **2.3** / fails / fails |

Arrow cannot infer the schema of `gh`: it infers timestamps from some strings,
then fails to convert others ("JSON conversion to struct<... timestamp[s] ...>
is not supported").

Reading from Python (simdjson_arrow through its C function, with ctypes,
importing the batches into pyarrow):

| | flat | gh | flat (inferred) |
|---|---|---|---|
| simdjson_arrow, 1 thread | 0.70 | 1.95 | 0.74 |
| polars 2.1.0 `read_ndjson`, 1 thread | 0.27 | 0.54 | 0.09 |
| pyarrow 26.0.0 `read_json`, 1 thread | 0.18 | 0.45 | 0.17 |
| simdjson_arrow, 32 threads | 13.8 | 27.5 | 13.4 |
| polars, 32 threads | 4.0 | 4.0 | 0.12 |
| pyarrow, 32 threads | 5.1 | 12.1 | 4.8 |

Inferring the schema of `gh` (all the fields), on one thread: simdjson_arrow
reads it at 0.34 GB/s, DuckDB 2.0.0 (`read_json_auto`) at 0.08 GB/s, polars at
0.025 GB/s; pyarrow fails (it infers timestamps, then cannot convert).

Writing, in C++:

| threads | flat | gh |
|---|---|---|
| 1 | 0.96 | 1.20 |
| 8 | 4.0 | 3.8 |
| 32 | 10.0 | 7.2 |
| 64 | 13.2 | 6.7 |

Writing from Python:

| | flat | gh |
|---|---|---|
| simdjson_arrow, 1 thread | 0.82 | 0.89 |
| polars 2.1.0 `write_ndjson`, 1 thread | 0.34 | 0.27 |
| DuckDB 2.0.0 `COPY ... (FORMAT json)`, 1 thread | 0.32 | 0.35 |
| orjson 3.13.0 on `Table.to_pylist()` | 0.05 | 0.07 |
| simdjson_arrow, 32 threads | 3.05 | 2.24 |
| polars, 32 threads | 1.66 | 0.84 |
| DuckDB, 32 threads | 2.78 | 0.84 |

From Python, the C functions and ctypes copy the data (the input when reading,
the output when writing), which costs most with many threads.

```sh
./build/bench_ndjson flat.ndjson flat 10 1 32     # file, schema (or infer), rounds, validate, threads
./build/bench_write flat.ndjson flat 10 32        # file, schema, rounds, threads
# Arrow C++, same arguments as bench_ndjson (CMAKE_PREFIX_PATH: where Arrow C++ is installed)
cmake -B build-arrow -DSIMDJSON_ARROW_BENCHMARK_ARROW_CPP=ON -DCMAKE_PREFIX_PATH=/opt/arrow
cmake --build build-arrow --target bench_arrow_cpp
./build-arrow/bench_arrow_cpp flat.ndjson flat 10 1 32
THREADS=32 python benchmarks/compare_python.py flat.ndjson:flat gharchive.ndjson:gh flat.ndjson:infer
THREADS=32 python benchmarks/compare_write.py flat.ndjson:flat gharchive.ndjson:gh
```

## License

Apache License 2.0. zmij (third_party/zmij) is under the MIT license.

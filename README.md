# simdjson_arrow

Reads newline-delimited JSON (NDJSON, JSON Lines) into [Apache Arrow](https://arrow.apache.org/)
record batches, with [simdjson](https://github.com/simdjson/simdjson), and writes
Arrow record batches as newline-delimited JSON.

The results are [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
structures (`ArrowSchema`, `ArrowArray`, `ArrowArrayStream`): Arrow C++, pyarrow,
polars, DuckDB or nanoarrow take them without copying. The library depends only on
simdjson (and on [zmij](https://github.com/vitaut/zmij), included, to write
floating-point numbers), not on Arrow. It is written in C++17.

Status: early. Inferring the schema and using several threads come next.

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

## Writing JSON

```cpp
ndjson_writer writer;  // or ndjson_writer writer(options), see ndjson_write_options
status s = writer.write(&schema, &batch);  // an ArrowSchema and a struct ArrowArray
// or writer.write(&stream) for all the batches of an ArrowArrayStream
std::string_view json = writer.view();  // or writer.release(&size): a buffer from malloc
```

We write one JSON object per row, one row per line. We accept the Arrow types
null, bool, the integers (8 to 64 bits, signed or not), float32, float64,
utf8, large utf8, utf8 view, struct, list, large list and fixed-size list, with
any offsets. Floating-point numbers are written as Python's `repr` writes them
(the shortest representation that gives back the number, `1.0` rather than
`1`); NaN and infinities, which JSON lacks, become `null`. A null field is
written as `"key":null`, or omitted (`write_nulls = false`). The C function
`simdjson_arrow_write_ndjson` writes an `ArrowArrayStream`, for bindings.

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

One core of an Intel Xeon Gold 6548N (Emerald Rapids), GCC 14, best of ten
runs, in GB/s of JSON. `flat`: 4 million flat records (numbers, strings,
booleans), 611 MB. `gh`: one hour of GH Archive events, 886 MB; we read six
fields and skip the rest, and we write these six fields (60 MB).

Reading, with an explicit schema:

| | flat | gh |
|---|---|---|
| simdjson_arrow | 0.72 | 2.00 |
| simdjson_arrow, skipped values not validated | 0.73 | 3.04 |
| polars 2.1.0 `read_ndjson` (Python) | 0.27 | 0.54 |
| pyarrow 26.0.0 `read_json` (Python) | 0.18 | 0.45 |

Writing:

| | flat | gh |
|---|---|---|
| simdjson_arrow | 0.99 | 1.18 |
| polars 2.1.0 `write_ndjson` (Python) | 0.46 | 0.48 |
| DuckDB 2.0.0 `COPY ... (FORMAT json)` (Python) | 0.30 | 0.31 |
| orjson 3.13.0 on `Table.to_pylist()` (Python) | 0.05 | 0.07 |

From Python, through the C functions and ctypes (which copy the data),
simdjson_arrow reads `flat` at 0.61 GB/s and `gh` at 1.45 GB/s, and writes them
at 0.83 GB/s and 0.87 GB/s.

```sh
./build/bench_ndjson flat.ndjson flat
./build/bench_write flat.ndjson flat
POLARS_MAX_THREADS=1 python benchmarks/compare_python.py flat.ndjson:flat gharchive.ndjson:gh
POLARS_MAX_THREADS=1 python benchmarks/compare_write.py flat.ndjson:flat gharchive.ndjson:gh
```

## License

Apache License 2.0. zmij (third_party/zmij) is under the MIT license.

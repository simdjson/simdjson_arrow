# simdjson_arrow

Reads newline-delimited JSON (NDJSON, JSON Lines) into [Apache Arrow](https://arrow.apache.org/)
record batches, with [simdjson](https://github.com/simdjson/simdjson).

The results are [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
structures (`ArrowSchema`, `ArrowArray`, `ArrowArrayStream`): Arrow C++, pyarrow,
polars, DuckDB or nanoarrow take them without copying. The library depends only on
simdjson, not on Arrow. It is written in C++17.

Status: early. We read NDJSON given a schema; writing JSON from Arrow and
inferring the schema come next.

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

One core of an Intel Xeon Gold 6548N (Emerald Rapids), GCC 14, explicit
schema, best of ten runs, in GB/s of JSON. `flat`: 4 million flat records
(numbers, strings, booleans), 611 MB. `gh`: one hour of GH Archive events,
886 MB, six fields kept, the rest skipped.

| | flat | gh |
|---|---|---|
| simdjson_arrow | 0.72 | 1.97 |
| simdjson_arrow, skipped values not validated | 0.73 | 3.04 |
| polars 2.1.0 `read_ndjson` (Python) | 0.27 | 0.54 |
| pyarrow 26.0.0 `read_json` (Python) | 0.18 | 0.45 |

From Python (through the C function, with ctypes, which copies the input),
simdjson_arrow reads `flat` at 0.61 GB/s and `gh` at 1.45 GB/s.

```sh
./build/bench_ndjson flat.ndjson flat
POLARS_MAX_THREADS=1 python benchmarks/compare_python.py flat.ndjson:flat gharchive.ndjson:gh
```

## License

Apache License 2.0.

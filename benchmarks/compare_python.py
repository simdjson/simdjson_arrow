"""Reads NDJSON files into Arrow tables, on one thread and with an explicit
schema, with simdjson_arrow (through its C API), pyarrow.json.read_json and
polars.read_ndjson. Reports the best of several runs in GB/s.

    POLARS_MAX_THREADS=1 python benchmarks/compare_python.py flat.ndjson:flat gharchive.ndjson:gh
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))

import pyarrow as pa
import pyarrow.json as pj

from compare_files import SCHEMAS
from test_pyarrow import ours

try:
    import polars as pl
except ImportError:
    pl = None

ROUNDS = int(os.environ.get("ROUNDS", "5"))


def best(fn):
    t = 1e300
    for _ in range(ROUNDS):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


print("file,reader,seconds,GBps")
for arg in sys.argv[1:]:
    path, name = arg.rsplit(":", 1)
    data = open(path, "rb").read()
    schema = SCHEMAS[name]
    readers = {
        "simdjson_arrow": lambda: ours(data, schema),
        "pyarrow": lambda: pj.read_json(
            pa.BufferReader(data), read_options=pj.ReadOptions(use_threads=False),
            parse_options=pj.ParseOptions(explicit_schema=schema,
                                          unexpected_field_behavior="ignore")),
    }
    if pl is not None:
        polars_schema = pl.from_arrow(schema.empty_table()).schema
        readers["polars"] = lambda: pl.read_ndjson(data, schema=polars_schema)
    for reader, fn in readers.items():
        t = best(fn)
        print(f"{os.path.basename(path)},{reader},{t:.4f},{len(data) / t / 1e9:.3f}", flush=True)

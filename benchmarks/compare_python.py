"""Reads NDJSON files into Arrow tables with simdjson_arrow (through its C API),
pyarrow.json.read_json and polars.read_ndjson, with THREADS threads (default 1),
given a schema or inferring it. Reports the best of several runs in GB/s.

    THREADS=32 python benchmarks/compare_python.py flat.ndjson:flat gharchive.ndjson:gh flat.ndjson:infer

Our reader copies the input (a Python bytes object has no padding), and
the copy is part of the time.
"""
import os
import sys
import time

THREADS = int(os.environ.get("THREADS", "1"))
os.environ["POLARS_MAX_THREADS"] = str(THREADS)  # before polars is imported

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
pa.set_cpu_count(THREADS)
pa.set_io_thread_count(THREADS)


def best(fn):
    t = 1e300
    for _ in range(ROUNDS):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


print("file,schema,reader,threads,seconds,GBps")
for arg in sys.argv[1:]:
    path, name = arg.rsplit(":", 1)
    data = open(path, "rb").read()
    schema = None if name == "infer" else SCHEMAS[name]
    read_options = pj.ReadOptions(use_threads=THREADS > 1)
    parse_options = (pj.ParseOptions() if schema is None else
                     pj.ParseOptions(explicit_schema=schema, unexpected_field_behavior="ignore"))
    readers = {
        "simdjson_arrow": lambda: ours(data, schema, threads=THREADS),
        "pyarrow": lambda: pj.read_json(pa.BufferReader(data), read_options=read_options,
                                        parse_options=parse_options),
    }
    if pl is not None:
        if schema is None:
            readers["polars"] = lambda: pl.read_ndjson(data, infer_schema_length=None)
        else:
            polars_schema = pl.from_arrow(schema.empty_table()).schema
            readers["polars"] = lambda: pl.read_ndjson(data, schema=polars_schema)
    for reader, fn in readers.items():
        try:
            t = best(fn)
            print(f"{os.path.basename(path)},{name},{reader},{THREADS},{t:.4f},"
                  f"{len(data) / t / 1e9:.3f}", flush=True)
        except Exception as e:
            print(f"{os.path.basename(path)},{name},{reader},{THREADS},failed,"
                  f"{type(e).__name__}", flush=True)

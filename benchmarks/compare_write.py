"""Writes Arrow tables as newline-delimited JSON, with THREADS threads (default 1), with
simdjson_arrow (through its C API), polars.DataFrame.write_ndjson, orjson (on
the rows of Table.to_pylist) and DuckDB (COPY ... TO a file, on tmpfs if
TMPDIR points to one). Reports the best of several runs in GB/s of JSON.

    THREADS=1 python benchmarks/compare_write.py flat.ndjson:flat gharchive.ndjson:gh
"""
import os
import sys
import tempfile
import time

THREADS = int(os.environ.get("THREADS", "1"))
os.environ["POLARS_MAX_THREADS"] = str(THREADS)  # before polars is imported

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))

from compare_files import SCHEMAS
from test_pyarrow import ours
from test_writer import write

ROUNDS = int(os.environ.get("ROUNDS", "5"))


def best(fn):
    t = 1e300
    for _ in range(ROUNDS):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


print("file,writer,threads,bytes,seconds,GBps")
for arg in sys.argv[1:]:
    path, name = arg.rsplit(":", 1)
    table = ours(open(path, "rb").read(), SCHEMAS[name])
    writers = {"simdjson_arrow": lambda: write(table, threads=THREADS)}
    try:
        import polars as pl
        frame = pl.from_arrow(table)
        writers["polars"] = lambda: frame.write_ndjson()
    except ImportError:
        pass
    try:
        import orjson
        writers["orjson (to_pylist)"] = lambda: b"\n".join(
            orjson.dumps(row) for row in table.to_pylist())
    except ImportError:
        pass
    try:
        import duckdb
        connection = duckdb.connect()
        connection.execute(f"SET threads = {THREADS}")
        connection.register("t", table)
        target = os.path.join(tempfile.gettempdir(), "simdjson_arrow_bench.json")

        def duck():
            connection.execute(f"COPY t TO '{target}' (FORMAT json)")
            return os.path.getsize(target)
        writers["duckdb"] = duck
    except ImportError:
        pass
    for writer, fn in writers.items():
        result = fn()
        size = result if isinstance(result, int) else len(result)
        t = best(fn)
        print(f"{os.path.basename(path)},{writer},{THREADS},{size},{t:.4f},{size / t / 1e9:.3f}",
              flush=True)

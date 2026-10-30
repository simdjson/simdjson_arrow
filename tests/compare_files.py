"""Reads whole NDJSON files with simdjson_arrow and with pyarrow.json.read_json
(same explicit schema) and checks that the tables are equal.

    python tests/compare_files.py flat.ndjson:flat gharchive.ndjson:gh
"""
import sys

import pyarrow as pa

from test_pyarrow import ours, theirs

SCHEMAS = {
    "flat": pa.schema([("id", pa.int64()), ("user", pa.int64()), ("price", pa.float64()),
                       ("lat", pa.float64()), ("lon", pa.float64()), ("name", pa.string()),
                       ("tag", pa.string()), ("active", pa.bool_()), ("count", pa.int64())]),
    "gh": pa.schema([("id", pa.string()), ("type", pa.string()),
                     ("actor", pa.struct([("id", pa.int64()), ("login", pa.string())])),
                     ("repo", pa.struct([("id", pa.int64()), ("name", pa.string())])),
                     ("public", pa.bool_()), ("created_at", pa.string())]),
}

if __name__ == "__main__":
    ok = True
    for arg in sys.argv[1:]:
        path, name = arg.rsplit(":", 1)
        data = open(path, "rb").read()
        got = ours(data, SCHEMAS[name])
        got.validate(full=True)
        expected = theirs(data, SCHEMAS[name])
        same = got.equals(expected)
        ok = ok and same
        print(f"{path}: {got.num_rows} rows, {got.num_columns} columns, "
              f"{len(got.to_batches())} batches: {'equal' if same else 'DIFFERENT'}")
    sys.exit(0 if ok else 1)

"""Tests of the NDJSON writer: we write pyarrow tables and parse the result
with the json module."""
import ctypes
import json
import math
import random

import pyarrow as pa
import pytest

from test_pyarrow import ArrowArrayStream, ReadError, lib, ours


class WriteError(Exception):
    pass


def write(table, write_nulls=True):
    """Writes a table (or a RecordBatchReader) with simdjson_arrow."""
    library = lib()
    library.simdjson_arrow_write_ndjson.restype = ctypes.c_int
    library.simdjson_arrow_write_ndjson.argtypes = [
        ctypes.POINTER(ArrowArrayStream), ctypes.c_int, ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p, ctypes.c_size_t]
    library.simdjson_arrow_free.argtypes = [ctypes.c_void_p]
    reader = table if isinstance(table, pa.RecordBatchReader) else pa.RecordBatchReader.from_batches(
        table.schema, table.to_batches())
    stream = ArrowArrayStream()
    reader._export_to_c(ctypes.addressof(stream))
    out = ctypes.c_void_p()
    length = ctypes.c_size_t()
    error = ctypes.create_string_buffer(1024)
    if library.simdjson_arrow_write_ndjson(ctypes.byref(stream), int(write_nulls),
                                           ctypes.byref(out), ctypes.byref(length), error,
                                           len(error)) != 0:
        raise WriteError(error.value.decode())
    try:
        return ctypes.string_at(out, length.value)
    finally:
        library.simdjson_arrow_free(out)


def to_float32(x):
    return ctypes.c_float(x).value


def normalize(value, t):
    """What we expect to read back from JSON for a value of Arrow type t."""
    if value is None:
        return None
    if pa.types.is_floating(t):
        if not math.isfinite(value):
            return None
        return to_float32(value) if pa.types.is_float32(t) else value
    if pa.types.is_struct(t):
        return {t.field(i).name: normalize(value.get(t.field(i).name), t.field(i).type)
                for i in range(t.num_fields)}
    if pa.types.is_list(t) or pa.types.is_large_list(t) or pa.types.is_fixed_size_list(t):
        return [normalize(v, t.value_type) for v in value]
    return value


def parsed(data, t):
    """Parses our output, rounding the float32 fields to float32."""
    def fix(value, t):
        if value is None:
            return None
        if pa.types.is_float32(t):
            return to_float32(value)
        if pa.types.is_struct(t):
            return {k: fix(v, t.field(k).type) for k, v in value.items()}
        if pa.types.is_list(t) or pa.types.is_large_list(t) or pa.types.is_fixed_size_list(t):
            return [fix(v, t.value_type) for v in value]
        return value
    return [fix(json.loads(line), t) for line in data.decode().splitlines()]


def check(table, write_nulls=True):
    data = write(table, write_nulls)
    assert data == b"" or data.endswith(b"\n")
    t = pa.struct(list(table.schema))
    expected = [normalize(row, t) for row in table.to_pylist()]
    if not write_nulls:
        expected = [drop_nulls(row) for row in expected]
    got = parsed(data, t)
    assert got == expected
    return data


def drop_nulls(value):
    """Without write_nulls, objects (at any depth) omit their null fields."""
    if isinstance(value, dict):
        return {k: drop_nulls(v) for k, v in value.items() if v is not None}
    if isinstance(value, list):
        return [drop_nulls(v) for v in value]
    return value


INTS = [pa.int8(), pa.uint8(), pa.int16(), pa.uint16(), pa.int32(), pa.uint32(), pa.int64(),
        pa.uint64()]


def test_integers():
    columns = {}
    for t in INTS:
        info = (0, 2 ** (t.bit_width) - 1) if "uint" in str(t) else (
            -2 ** (t.bit_width - 1), 2 ** (t.bit_width - 1) - 1)
        columns[str(t)] = pa.array([info[0], None, 0, info[1], 7], type=t)
    check(pa.table(columns))


def test_floats():
    values = [0.1, -0.0, 1.0, 1e21, 1e-7, 5e-324, 1.7976931348623157e308, float("nan"),
              float("inf"), float("-inf"), None, 123456.789, 2 ** 53 + 1.0]
    data = check(pa.table({"d": pa.array(values, pa.float64()),
                           "f": pa.array(values, pa.float32())}))
    assert b"NaN" not in data and b"Infinity" not in data
    # shortest representations
    assert write(pa.table({"f": pa.array([0.1, 1.0, 3.4028235e38], pa.float32())})) == \
        b'{"f":0.1}\n{"f":1.0}\n{"f":3.4028235e+38}\n'


def test_doubles_like_repr():
    # We write doubles as Python's repr (and json.dumps) does.
    import struct
    rng = random.Random(3)
    values = [0.0, -0.0, 1.0, -1.0, 123.0, 1e15, 1e16, 9999999999999998.0, 2.0 ** 53, 1e-4,
              1e-5, 0.0001234, 1e22, 1e23, 5e-324, 2.2250738585072014e-308, 0.1, 0.2, 1 / 3]
    while len(values) < 20000:
        x = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        if math.isfinite(x):
            values.append(x)
    values += [rng.uniform(-1e6, 1e6) for _ in range(1000)] + [float(rng.randint(-10**17, 10**17)) for _ in range(1000)]
    data = write(pa.table({"d": pa.array(values, pa.float64())}))
    assert data.decode().splitlines() == ['{"d":%r}' % x for x in values]


def test_strings():
    values = ["", "plain", 'quote " backslash \\ slash /', "\n\r\t\b\f\x01\x1f\x7f",
              "é中\U0001f600", None, "x" * 100, "twelve chars"]
    table = pa.table({"u": pa.array(values, pa.string()), "U": pa.array(values, pa.large_string()),
                      "v": pa.array(values, pa.string_view())})
    check(table)


def test_escapes_everywhere():
    # every character that needs escaping, at every position around the
    # 8-byte words, next to non-ASCII characters
    specials = [chr(c) for c in range(0x20)] + ['"', "\\"]
    values = []
    for c in specials:
        for before in range(0, 18):
            values.append("a" * before + c + "é" * (before % 3) + "b" * (17 - before))
    values.append("".join(specials) * 3)
    values.append("no escapes here, but more than eight bytes 中\U0001f600 " * 20)
    data = check(pa.table({"s": values}))
    assert all(0x20 <= b or b == 0x0a for b in data)  # control characters are escaped
    assert b'"\\u001f' in write(pa.table({"s": ["\x1f"]}))


def test_keys_and_null_type():
    table = pa.table({'a "quoted" key': [1, 2], "é": ["x", None], "n": pa.nulls(2)})
    check(table)
    assert check(table, write_nulls=False).splitlines()[1] == b'{"a \\"quoted\\" key":2}'


def test_nested():
    t = pa.struct([("x", pa.int64()), ("y", pa.list_(pa.string())),
                   ("z", pa.struct([("deep", pa.large_list(pa.float64()))]))])
    rows = [{"x": 1, "y": ["a", None], "z": {"deep": [1.5, None]}}, None,
            {"x": None, "y": [], "z": None}, {"x": 3, "y": None, "z": {"deep": []}}]
    table = pa.table({
        "s": pa.array(rows, t),
        "l": pa.array([[[1], [2, 3]], None, [[]], [None, [4]]], pa.list_(pa.list_(pa.int32()))),
        "w": pa.array([[1, 2], None, [3, None], [5, 6]], pa.list_(pa.int64(), 2)),
        "b": pa.array([True, False, None, True]),
    })
    check(table)
    check(table, write_nulls=False)


def test_slices_and_offsets():
    table = pa.table({
        "i": pa.array(range(100), pa.int64()),
        "s": pa.array([str(i) if i % 3 else None for i in range(100)]),
        "l": pa.array([[i] * (i % 4) for i in range(100)], pa.list_(pa.int64())),
        "o": pa.array([{"a": i, "b": [str(i)]} for i in range(100)],
                      pa.struct([("a", pa.int64()), ("b", pa.list_(pa.string()))])),
        "v": pa.array([("v" * (i % 20)) for i in range(100)], pa.string_view()),
    })
    for start, length in [(0, 100), (1, 50), (37, 13), (99, 1), (50, 0)]:
        check(table.slice(start, length))
    # children with their own offsets, under a struct with an offset
    a = pa.array(range(20), pa.int64()).slice(5, 10)
    b = pa.array([str(i) for i in range(30)]).slice(17, 10)
    s = pa.StructArray.from_arrays([a, b], names=["a", "b"]).slice(3, 5)
    check(pa.table({"s": s}))
    values = pa.array(range(50), pa.int64()).slice(10)
    lists = pa.ListArray.from_arrays(pa.array([0, 2, 2, 5], pa.int32()), values).slice(1)
    check(pa.table({"l": lists}))


def test_batches():
    batches = [pa.record_batch({"i": [i, i + 1]}) for i in range(0, 10, 2)]
    reader = pa.RecordBatchReader.from_batches(batches[0].schema, batches)
    assert write(reader) == b"".join(b'{"i":%d}\n' % i for i in range(10))
    assert write(pa.table({"i": pa.array([], pa.int64())})) == b""


def test_round_trip():
    rng = random.Random(7)
    schema = pa.schema([("i", pa.int64()), ("u", pa.uint64()), ("d", pa.float64()),
                        ("s", pa.string()), ("b", pa.bool_()),
                        ("o", pa.struct([("x", pa.int64()), ("y", pa.list_(pa.string()))]))])
    rows = []
    for _ in range(3000):
        rows.append({
            "i": rng.choice([None, rng.randint(-2 ** 63, 2 ** 63 - 1)]),
            "u": rng.choice([None, rng.randint(0, 2 ** 64 - 1)]),
            "d": rng.choice([None, rng.uniform(-1e300, 1e300), rng.random(), float(rng.randint(-9, 9))]),
            "s": rng.choice([None, "".join(rng.choice('ab"\\\né\U0001f600') for _ in range(rng.randint(0, 9)))]),
            "b": rng.choice([None, True, False]),
            "o": rng.choice([None, {"x": rng.randint(0, 5), "y": [rng.choice(["p", None])] * rng.randint(0, 3)}]),
        })
    table = pa.Table.from_pylist(rows, schema=schema)
    data = check(table)
    assert ours(data, schema).equals(table)
    assert ours(write(table, write_nulls=False), schema).equals(table)


@pytest.mark.parametrize("column", [
    pa.array([1], pa.timestamp("s")),
    pa.array(["a"]).dictionary_encode(),
    pa.array([b"x"], pa.binary()),
])
def test_unsupported(column):
    with pytest.raises(WriteError, match="not supported|unsupported"):
        write(pa.table({"c": column}))

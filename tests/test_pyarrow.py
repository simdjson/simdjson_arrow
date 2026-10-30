"""Compares simdjson_arrow with pyarrow.json.read_json, given the same schema.

    SIMDJSON_ARROW_LIB=build/libsimdjson_arrow_c.so pytest tests
(by default, we look for the library in build/).
"""
import ctypes
import glob
import io
import json
import os
import random

import pyarrow as pa
import pyarrow.json as pj
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_library():
    if "SIMDJSON_ARROW_LIB" in os.environ:
        return os.environ["SIMDJSON_ARROW_LIB"]
    found = glob.glob(os.path.join(ROOT, "build", "libsimdjson_arrow_c.*"))
    if not found:
        pytest.skip("build the library first (cmake -B build && cmake --build build)")
    return found[0]


class ArrowSchema(ctypes.Structure):
    pass


ArrowSchema._fields_ = [
    ("format", ctypes.c_char_p), ("name", ctypes.c_char_p), ("metadata", ctypes.c_char_p),
    ("flags", ctypes.c_int64), ("n_children", ctypes.c_int64),
    ("children", ctypes.POINTER(ctypes.POINTER(ArrowSchema))),
    ("dictionary", ctypes.POINTER(ArrowSchema)),
    ("release", ctypes.CFUNCTYPE(None, ctypes.POINTER(ArrowSchema))),
    ("private_data", ctypes.c_void_p),
]


class ArrowArrayStream(ctypes.Structure):
    _fields_ = [("get_schema", ctypes.c_void_p), ("get_next", ctypes.c_void_p),
                ("get_last_error", ctypes.c_void_p), ("release", ctypes.c_void_p),
                ("private_data", ctypes.c_void_p)]


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = ctypes.CDLL(find_library())
        _lib.simdjson_arrow_read_ndjson.restype = ctypes.c_int
        _lib.simdjson_arrow_read_ndjson.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.POINTER(ArrowSchema),
            ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ArrowArrayStream),
            ctypes.c_char_p, ctypes.c_size_t]
    return _lib


class ReadError(Exception):
    pass


def ours(data, schema, block_size=0, error_on_unknown_fields=False, validate_skipped=True):
    c_schema = ArrowSchema()
    schema._export_to_c(ctypes.addressof(c_schema))
    stream = ArrowArrayStream()
    error = ctypes.create_string_buffer(1024)
    try:
        rc = lib().simdjson_arrow_read_ndjson(
            data, len(data), len(data), ctypes.byref(c_schema), block_size,
            int(error_on_unknown_fields), int(validate_skipped), ctypes.byref(stream), error,
            len(error))
    finally:
        c_schema.release(ctypes.byref(c_schema))
    if rc != 0:
        raise ReadError(error.value.decode())
    reader = pa.RecordBatchReader._import_from_c(ctypes.addressof(stream))
    return reader.read_all()


def theirs(data, schema):
    return pj.read_json(
        io.BytesIO(data),
        read_options=pj.ReadOptions(use_threads=False),
        parse_options=pj.ParseOptions(explicit_schema=schema,
                                      unexpected_field_behavior="ignore"))


def same(data, schema, **kwargs):
    expected = theirs(data, schema)
    got = ours(data, schema, **kwargs)
    got.validate(full=True)
    assert got.schema == expected.schema
    assert got.equals(expected), f"\nours:   {got.to_pylist()}\npyarrow: {expected.to_pylist()}"


SCALARS = pa.schema([("i", pa.int64()), ("u", pa.uint64()), ("d", pa.float64()),
                     ("s", pa.string()), ("b", pa.bool_()), ("n", pa.null())])

NESTED = pa.schema([
    ("o", pa.struct([("x", pa.int64()), ("y", pa.list_(pa.string()))])),
    ("l", pa.list_(pa.struct([("a", pa.float64())]))),
    ("ll", pa.list_(pa.list_(pa.int64()))),
])


@pytest.mark.parametrize("data", [
    b'{"i": 1, "u": 2, "d": 3.5, "s": "x", "b": true, "n": null}',
    b'{"i": -9223372036854775808, "u": 18446744073709551615, "d": -0.0}',
    b'{"d": 1e308, "s": "\\u00e9\\n\\"\\\\\\/", "b": false}\n{}\n{"s": ""}',
    b'{"d": 123456789012345678901234567890, "s": "\xf0\x9f\x98\x80"}',
    b'{"d": 5}\n{"d": -5}\n{"d": 0.1}\n{"d": 1E-7}',
    b'\n\n  {"i": 1}  \n\n\n{"i": 2}\r\n{"i": 3}',
    b'{"i": null, "u": null, "d": null, "s": null, "b": null, "n": null}',
    b'{"zz": [1, {"a": [true, null]}], "i": 4, "yy": "skip", "xx": {"q": 1e5}}',
    b'{"s": "a"}{"s": "b"} {"s": "c"}',
    b'{"b": true, "s": "y", "i": 3}\n{"s": "z", "b": false}',
    b'{"zz": "\\u00e9\\"", "zy": {"\\u0041": [1e-5, -0.5E+2, 0]}, "i": 1, "zx": 123456789012345678901234567890}',
])
def test_scalars(data):
    same(data, SCALARS)


@pytest.mark.parametrize("data", [
    b'{"o": {"x": 1, "y": ["a", null, "b"]}, "l": [{"a": 1.5}, {"a": null}, {}], "ll": [[1], [], [2, 3]]}',
    b'{"o": null, "l": null, "ll": null}\n{}\n{"o": {}, "l": [], "ll": [[]]}',
    b'{"o": {"y": [], "x": 7, "z": {"deep": [1]}}, "l": [null], "ll": [null, [4]]}',
])
def test_nested(data):
    same(data, NESTED)


def random_value(rng, depth=0):
    r = rng.random()
    if depth > 3 or r < 0.5:
        return rng.choice([None, True, False, rng.randint(-10**6, 10**6), rng.uniform(-1e6, 1e6),
                           "".join(rng.choice("ab\"\\é\n ") for _ in range(rng.randint(0, 6)))])
    if r < 0.75:
        return [random_value(rng, depth + 1) for _ in range(rng.randint(0, 4))]
    return {rng.choice("pqrs"): random_value(rng, depth + 1) for _ in range(rng.randint(0, 4))}


def test_random_records():
    rng = random.Random(42)
    lines = []
    for _ in range(5000):
        record = {}
        for key in ["i", "d", "s", "b", "o", "extra"]:
            if rng.random() < 0.8:
                record[key] = None if rng.random() < 0.1 else {
                    "i": rng.randint(-2**63, 2**63 - 1),
                    "d": rng.choice([rng.uniform(-1e300, 1e300), rng.randint(-2**53, 2**53)]),
                    "s": "".join(rng.choice("xy\"\\é中\U0001f600\t") for _ in range(rng.randint(0, 12))),
                    "b": rng.random() < 0.5,
                    "o": {"x": rng.randint(0, 9), "y": [rng.choice(["u", None])] * rng.randint(0, 3)},
                    "extra": random_value(rng),
                }[key]
        lines.append(json.dumps(record, ensure_ascii=rng.random() < 0.5))
    data = "\n".join(lines).encode()
    schema = pa.schema([("i", pa.int64()), ("d", pa.float64()), ("s", pa.string()),
                        ("b", pa.bool_()),
                        ("o", pa.struct([("x", pa.int64()), ("y", pa.list_(pa.string()))]))])
    same(data, schema)
    for block_size in [100, 4096]:
        got = ours(data, schema, block_size=block_size)
        assert got.equals(theirs(data, schema))


@pytest.mark.parametrize("data", [
    b'{"i": "1"}', b'{"i": 1.5}', b'{"i": 1e3}', b'{"i": 9223372036854775808}',
    b'{"u": -1}', b'{"s": 1}', b'{"b": 0}', b'{"n": 1}', b'{"i": 01}', b'{"d": 1.}',
    b'{"d": -}', b'{"i": tru}', b'{"i": 1, "i": 2}', b'[1]', b'5', b'{"i": 1',
    b'{"i": [1]}', b'{"s": {"a": 1}}', b'{"zz": [1, tru]}', b'{"zz": {"a": 01}}',
    b'{"i": 1}\n{"i": 2}\n{"i": "x"}', b'{"i": 1} x', b'{"s": "\\x"}',
    # skipped values are validated
    b'{"zz": "\\x"}', b'{"zz": {"\\x": 1}}', b'{"zz": [1.]}', b'{"zz": -}', b'{"zz": 1e}',
    b'{"zz": [nul]}', b'{"zz": {"a": [{"b": "\\u12"}]}}',
])
def test_errors(data):
    with pytest.raises(pa.ArrowInvalid):
        theirs(data, SCALARS)
    with pytest.raises(ReadError):
        ours(data, SCALARS)


def test_unknown_fields():
    with pytest.raises(ReadError, match="absent from the schema"):
        ours(b'{"i": 1, "zz": 2}', SCALARS, error_on_unknown_fields=True)
    # without validation, skipped values are only checked for structure
    ours(b'{"i": 1, "zz": {"a": 01}}', SCALARS, validate_skipped=False)


def test_non_nullable():
    schema = pa.schema([pa.field("x", pa.int64(), nullable=False)])
    assert ours(b'{"x": 1}', schema).schema == schema
    with pytest.raises(ReadError, match="missing non-nullable field"):
        ours(b'{}', schema)
    with pytest.raises(ReadError, match="null in a non-nullable field"):
        ours(b'{"x": null}', schema)


def test_empty_input():
    for data in [b"", b"\n", b"   \n  "]:
        table = ours(data, SCALARS)
        assert table.num_rows == 0 and table.schema == SCALARS


def test_unsupported_type():
    with pytest.raises(ReadError, match="unsupported Arrow type"):
        ours(b'{}', pa.schema([("t", pa.timestamp("s"))]))

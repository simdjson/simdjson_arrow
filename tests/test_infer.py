"""Schema inference and threads: we compare with pyarrow.json.read_json."""
import io
import json
import random

import pyarrow as pa
import pyarrow.json as pj
import pytest

from test_pyarrow import ReadError, ours


def inferred_by_pyarrow(data):
    return pj.read_json(io.BytesIO(data), read_options=pj.ReadOptions(use_threads=False))


def same(data, **kwargs):
    # pyarrow 26.0.0 infers the schema we expect, but its values can be wrong
    # when it infers (see test_list_with_leading_null): we compare our values
    # with those it reads given the schema.
    schema = inferred_by_pyarrow(data).schema
    got = ours(data, **kwargs)
    got.validate(full=True)
    assert got.schema == schema, f"\nours:    {got.schema}\npyarrow: {schema}"
    expected = pj.read_json(
        io.BytesIO(data), read_options=pj.ReadOptions(use_threads=False),
        parse_options=pj.ParseOptions(explicit_schema=schema))
    assert got.equals(expected)
    return got


def test_list_with_leading_null():
    # pyarrow 26.0.0 returns [[2.0, 3.5], [0.0]] (the arrays it builds are
    # invalid); Arrow's main branch (2026-10-29) is right.
    data = b'{"z": [null, 2]}\n{"z": [3.5]}'
    assert ours(data).to_pylist() == [{"z": [None, 2.0]}, {"z": [3.5]}]
    same(data)


@pytest.mark.parametrize("data", [
    b'{"a": 1, "b": 2.5, "c": "x", "d": true, "e": null}',
    b'{"a": 1}\n{"a": 2.5}\n{"a": null}\n{"a": 3}',
    b'{"a": null}\n{"a": null}\n{"a": 7}',
    b'{"a": null}\n{"a": null}',
    b'{"a": 1}\n{"b": "x"}\n{"c": [1, 2]}\n{"a": 2, "d": {"e": 1}}',
    b'{"a": 9223372036854775807}\n{"a": 9223372036854775808}',
    b'{"a": 123456789012345678901234567890}\n{"a": 1}',
    b'{"l": []}\n{"l": [null]}\n{"l": [1, 2.5]}\n{"l": null}',
    b'{"l": [[1], [], [2, 3]]}\n{"l": [[4.5]]}',
    b'{"o": {}}\n{"o": {"x": 1}}\n{"o": {"y": "z", "x": null}}\n{"o": null}',
    b'{"o": {"p": {"q": [{"r": 1}, {"s": true}]}}}\n{"o": {"p": {"q": [{"r": 2.5, "t": "u"}]}}}',
    b'{"b": "y", "a": 1}\n{"a": 2, "b": "z", "c": false}',
    b'{"\\u0061": 1, "b\\"c": 2}\n{"a": 3}',
    b'{}\n{}\n{"a": 1}',
])
def test_inference(data):
    same(data)
    same(data, block_size=8)  # many batches, merged at the end


def random_records(rng, n):
    """Records whose keys keep one kind of value (so that pyarrow can read them),
    with nulls and missing keys."""
    def value(kind, depth=0):
        if rng.random() < 0.15:
            return None
        if kind == "int":
            return rng.randint(-10**12, 10**12)
        if kind == "number":
            return rng.choice([rng.randint(-100, 100), rng.uniform(-1e6, 1e6), 2**70])
        if kind == "string":
            return "".join(rng.choice('ab"\\é\U0001f600 ') for _ in range(rng.randint(0, 8)))
        if kind == "bool":
            return rng.random() < 0.5
        if kind == "list":
            return [value("number") for _ in range(rng.randint(0, 3))]
        if kind == "struct":
            return {k: value(v, depth + 1) for k, v in [("x", "int"), ("y", "string"), ("z", "list")]
                    if rng.random() < 0.7}
        if kind == "records":
            return [value("struct", depth + 1) for _ in range(rng.randint(0, 2))]
    kinds = {"i": "int", "n": "number", "s": "string", "b": "bool", "l": "list", "o": "struct",
             "r": "records", "late": "string"}
    records = []
    for r in range(n):
        keys = [k for k in kinds if rng.random() < 0.7 and (k != "late" or r > n // 2)]
        rng.shuffle(keys)
        records.append({k: value(kinds[k]) for k in keys})
    return records


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_random_inference(seed):
    rng = random.Random(seed)
    data = "\n".join(json.dumps(r) for r in random_records(rng, 3000)).encode()
    for block_size in [0, 300, 5000]:
        same(data, block_size=block_size)


@pytest.mark.parametrize("threads", [1, 2, 3, 8, 16])
def test_threads(threads):
    rng = random.Random(10 + threads)
    data = "\n".join(json.dumps(r) for r in random_records(rng, 4000)).encode()
    for block_size in [200, 1000, 64 * 1024]:
        got = same(data, block_size=block_size, threads=threads)
        schema = got.schema
        explicit = ours(data, schema, block_size=block_size, threads=threads)
        assert explicit.equals(got)


@pytest.mark.parametrize("data", [
    b'{"a": 1}\n{"a": "x"}', b'{"a": "x"}\n{"a": 1}', b'{"a": [1]}\n{"a": {"b": 1}}',
    b'{"a": true}\n{"a": 0}', b'{"a": {"b": 1}}\n{"a": {"b": "c"}}', b'{"a": [1, "x"]}',
])
def test_conflicts(data):
    with pytest.raises(pa.ArrowInvalid):
        inferred_by_pyarrow(data)
    for block_size in [0, 8]:
        with pytest.raises(ReadError):
            ours(data, block_size=block_size)


def test_parallel_errors_like_serial():
    lines = [b'{"a": %d, "b": "x"}' % i for i in range(5000)]
    lines[3777] = b'{"a": 1, "b": 5}'
    lines[4500] = b'{"a": tru}'
    data = b"\n".join(lines)
    schema = pa.schema([("a", pa.int64()), ("b", pa.string())])
    messages = set()
    for threads in [1, 4, 16]:
        for s in [schema, None]:
            with pytest.raises(ReadError) as e:
                ours(data, s, block_size=1000, threads=threads)
            messages.add((s is None, str(e.value)))
    assert len(messages) == 2  # one message with a schema, one without
    assert all("row 3777" in m for _, m in messages)


def test_records_spanning_lines():
    # With threads, we cut the input at line ends: a record that spans lines
    # makes us read again with one thread.
    records = [{"a": i, "b": [i, i + 1], "c": {"d": str(i)}} for i in range(2000)]
    data = "\n".join(json.dumps(r, indent=2) for r in records).encode()
    expected = ours(data, block_size=500)
    assert expected.num_rows == 2000
    assert ours(data, block_size=500, threads=8).equals(expected)


def test_empty():
    for data in [b"", b"\n\n", b"  "]:
        for threads in [1, 4]:
            table = ours(data, threads=threads)
            assert table.num_rows == 0 and table.num_columns == 0

# Writes a flat, regular newline-delimited JSON file: one record per line with
# integers, floats, short strings and booleans.
#   python3 gendata.py out.ndjson [records]
import json
import random
import sys

random.seed(1234)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 4_000_000
words = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"]
with open(sys.argv[1], "w") as f:
    for i in range(n):
        record = {
            "id": i,
            "user": random.randrange(1_000_000),
            "price": round(random.uniform(0, 1000), 2),
            "lat": random.uniform(-90, 90),
            "lon": random.uniform(-180, 180),
            "name": random.choice(words) + "-" + str(random.randrange(10_000)),
            "tag": random.choice(words),
            "active": random.random() < 0.5,
            "count": random.randrange(100),
        }
        f.write(json.dumps(record, separators=(",", ":")) + "\n")

// Reads a newline-delimited JSON file into Arrow record batches and reports
// the best throughput over several runs.
//   bench_ndjson <file.ndjson> <schema: flat | gh> [rounds] [validate: 1 | 0]
// flat: the records of gendata.py in the blog benchmark (9 columns);
// gh: six fields of the GH Archive events, the other fields are skipped.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "simdjson_arrow.h"

using namespace simdjson_arrow;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s file.ndjson flat|gh [rounds] [validate]\n", argv[0]);
    return EXIT_FAILURE;
  }
  const std::string which = argv[2];
  const int rounds = argc > 3 ? std::atoi(argv[3]) : 10;
  ndjson_options options;
  options.validate_skipped = argc > 4 ? std::atoi(argv[4]) != 0 : true;
  field schema;
  if (which == "flat") {
    schema = make_schema({make_field("id", type::int64), make_field("user", type::int64),
                          make_field("price", type::float64), make_field("lat", type::float64),
                          make_field("lon", type::float64), make_field("name", type::utf8),
                          make_field("tag", type::utf8), make_field("active", type::boolean),
                          make_field("count", type::int64)});
  } else if (which == "gh") {
    schema = make_schema(
        {make_field("id", type::utf8), make_field("type", type::utf8),
         make_struct("actor", {make_field("id", type::int64), make_field("login", type::utf8)}),
         make_struct("repo", {make_field("id", type::int64), make_field("name", type::utf8)}),
         make_field("public", type::boolean), make_field("created_at", type::utf8)});
  } else {
    std::fprintf(stderr, "unknown schema %s\n", which.c_str());
    return EXIT_FAILURE;
  }
  simdjson::padded_string json;
  if (simdjson::padded_string::load(argv[1]).get(json)) {
    std::fprintf(stderr, "cannot load %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  double best = 1e300;
  int64_t rows = 0;
  size_t n_batches = 0;
  for (int r = 0; r < rounds; r++) {
    std::vector<ArrowArray> batches;
    auto t0 = std::chrono::steady_clock::now();
    status s = read_ndjson(schema, json, &batches, options);
    auto t1 = std::chrono::steady_clock::now();
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.message.c_str());
      return EXIT_FAILURE;
    }
    rows = 0;
    for (ArrowArray& b : batches) {
      rows += b.length;
      b.release(&b);
    }
    n_batches = batches.size();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  // file,schema,validate,bytes,rows,batches,seconds,GB/s
  std::printf("%s,%s,%d,%zu,%lld,%zu,%.4f,%.3f\n", argv[1], which.c_str(),
              options.validate_skipped ? 1 : 0, json.size(), static_cast<long long>(rows),
              n_batches, best, json.size() / best / 1e9);
  return EXIT_SUCCESS;
}

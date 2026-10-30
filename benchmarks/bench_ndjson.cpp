// Reads a newline-delimited JSON file into Arrow record batches and reports
// the best throughput over several runs.
//   bench_ndjson <file.ndjson> <schema: flat | gh | infer> [rounds] [validate: 1 | 0] [threads]
// The schemas are in schemas.h; with infer, we infer the schema.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "schemas.h"
#include "simdjson_arrow.h"

using namespace simdjson_arrow;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s file.ndjson flat|gh|infer [rounds] [validate] [threads]\n",
                 argv[0]);
    return EXIT_FAILURE;
  }
  const std::string which = argv[2];
  const int rounds = argc > 3 ? std::atoi(argv[3]) : 10;
  ndjson_options options;
  options.validate_skipped = argc > 4 ? std::atoi(argv[4]) != 0 : true;
  options.threads = argc > 5 ? static_cast<size_t>(std::atoi(argv[5])) : 1;
  const bool infer = which == "infer";
  field schema;
  if (!infer && !benchmark_schema(which, &schema)) {
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
    field inferred;
    status s = infer ? read_ndjson(json, &inferred, &batches, options)
                     : read_ndjson(schema, json, &batches, options);
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
  // file,schema,validate,threads,bytes,rows,batches,seconds,GB/s
  std::printf("%s,%s,%d,%zu,%zu,%lld,%zu,%.4f,%.3f\n", argv[1], which.c_str(),
              options.validate_skipped ? 1 : 0, options.threads, json.size(),
              static_cast<long long>(rows), n_batches, best, json.size() / best / 1e9);
  return EXIT_SUCCESS;
}

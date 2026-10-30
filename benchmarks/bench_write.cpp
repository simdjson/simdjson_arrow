// Reads a newline-delimited JSON file into Arrow record batches (untimed), then
// writes the batches back as newline-delimited JSON and reports the best
// throughput over several runs, in GB/s of JSON written.
//   bench_write <file.ndjson> <schema: flat | gh> [rounds] [threads]
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
    std::fprintf(stderr, "usage: %s file.ndjson flat|gh [rounds] [threads]\n", argv[0]);
    return EXIT_FAILURE;
  }
  const int rounds = argc > 3 ? std::atoi(argv[3]) : 10;
  ndjson_write_options options;
  options.threads = argc > 4 ? static_cast<size_t>(std::atoi(argv[4])) : 1;
  field schema;
  if (!benchmark_schema(argv[2], &schema)) {
    std::fprintf(stderr, "unknown schema %s\n", argv[2]);
    return EXIT_FAILURE;
  }
  simdjson::padded_string json;
  if (simdjson::padded_string::load(argv[1]).get(json)) {
    std::fprintf(stderr, "cannot load %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  std::vector<ArrowArray> batches;
  status s = read_ndjson(schema, json, &batches);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  ArrowSchema exported;
  if (!export_schema(schema, &exported).ok()) return EXIT_FAILURE;
  double best = 1e300;
  size_t bytes = 0;
  for (int r = 0; r < rounds; r++) {
    auto t0 = std::chrono::steady_clock::now();
    ndjson_writer writer(options);
    std::vector<const ArrowArray*> pointers;
    for (ArrowArray& b : batches) pointers.push_back(&b);
    s = writer.write(&exported, pointers.data(), pointers.size());
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.message.c_str());
      return EXIT_FAILURE;
    }
    char* out = writer.release(&bytes);
    auto t1 = std::chrono::steady_clock::now();
    std::free(out);
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  for (ArrowArray& b : batches) b.release(&b);
  exported.release(&exported);
  // file,schema,threads,bytes written,seconds,GB/s
  std::printf("%s,%s,%zu,%zu,%.4f,%.3f\n", argv[1], argv[2], options.threads, bytes, best,
              bytes / best / 1e9);
  return EXIT_SUCCESS;
}

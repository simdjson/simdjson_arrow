// Reads a newline-delimited JSON file into an Arrow table with Arrow C++'s
// reader (arrow::json::TableReader), for comparison with bench_ndjson. Same
// arguments and output as bench_ndjson:
//   bench_arrow_cpp <file.ndjson> <schema: flat | gh | infer> [rounds] [validate] [threads]
// validate is ignored (Arrow always checks the skipped values). With a schema,
// Arrow ignores the fields absent from it, as we do.
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/json/api.h>
#include <arrow/util/thread_pool.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

static std::shared_ptr<arrow::Schema> benchmark_schema(const std::string& name) {
  using arrow::field;
  if (name == "flat") {
    return arrow::schema({field("id", arrow::int64()), field("user", arrow::int64()),
                          field("price", arrow::float64()), field("lat", arrow::float64()),
                          field("lon", arrow::float64()), field("name", arrow::utf8()),
                          field("tag", arrow::utf8()), field("active", arrow::boolean()),
                          field("count", arrow::int64())});
  }
  if (name == "gh") {
    return arrow::schema(
        {field("id", arrow::utf8()), field("type", arrow::utf8()),
         field("actor", arrow::struct_({field("id", arrow::int64()), field("login", arrow::utf8())})),
         field("repo", arrow::struct_({field("id", arrow::int64()), field("name", arrow::utf8())})),
         field("public", arrow::boolean()), field("created_at", arrow::utf8())});
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s file.ndjson flat|gh|infer [rounds] [validate] [threads]\n",
                 argv[0]);
    return EXIT_FAILURE;
  }
  const std::string which = argv[2];
  const int rounds = argc > 3 ? std::atoi(argv[3]) : 10;
  const int threads = argc > 5 ? std::atoi(argv[5]) : 1;
  auto parse_options = arrow::json::ParseOptions::Defaults();
  if (which != "infer") {
    parse_options.explicit_schema = benchmark_schema(which);
    if (parse_options.explicit_schema == nullptr) {
      std::fprintf(stderr, "unknown schema %s\n", which.c_str());
      return EXIT_FAILURE;
    }
    parse_options.unexpected_field_behavior = arrow::json::UnexpectedFieldBehavior::Ignore;
  }
  auto read_options = arrow::json::ReadOptions::Defaults();
  read_options.use_threads = threads > 1;
  if (threads > 1 && !arrow::SetCpuThreadPoolCapacity(threads).ok()) return EXIT_FAILURE;
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot load %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto buffer = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>(data.data()),
                                                static_cast<int64_t>(data.size()));
  double best = 1e300;
  int64_t rows = 0;
  size_t n_batches = 0;
  for (int r = 0; r < rounds; r++) {
    auto t0 = std::chrono::steady_clock::now();
    auto reader = arrow::json::TableReader::Make(
        arrow::default_memory_pool(), std::make_shared<arrow::io::BufferReader>(buffer),
        read_options, parse_options);
    if (!reader.ok()) {
      std::fprintf(stderr, "%s\n", reader.status().ToString().c_str());
      return EXIT_FAILURE;
    }
    auto table = (*reader)->Read();
    auto t1 = std::chrono::steady_clock::now();
    if (!table.ok()) {
      std::fprintf(stderr, "%s\n", table.status().ToString().c_str());
      return EXIT_FAILURE;
    }
    rows = (*table)->num_rows();
    n_batches = (*table)->num_columns() > 0 ? (*table)->column(0)->num_chunks() : 0;
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  // file,schema,validate,threads,bytes,rows,batches,seconds,GB/s
  std::printf("%s,%s,1,%d,%zu,%lld,%zu,%.4f,%.3f\n", argv[1], which.c_str(), threads,
              data.size(), static_cast<long long>(rows), n_batches, best,
              data.size() / best / 1e9);
  return EXIT_SUCCESS;
}

// With Arrow C++: reads a newline-delimited JSON file into an arrow::Table
// (inferring the schema), then writes the table back as newline-delimited JSON.
// The data crosses between the two libraries without a copy, through the Arrow
// C stream interface.
//   arrow_cpp <file.ndjson> [threads]
#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "simdjson_arrow.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s file.ndjson [threads]\n", argv[0]);
    return EXIT_FAILURE;
  }
  simdjson_arrow::ndjson_options options;
  options.threads = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1;
  simdjson::padded_string json;
  if (simdjson::padded_string::load(argv[1]).get(json)) {
    std::fprintf(stderr, "cannot load %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  // NDJSON to an arrow::Table.
  simdjson_arrow::field schema;
  std::vector<ArrowArray> batches;
  simdjson_arrow::status s = simdjson_arrow::read_ndjson(json, &schema, &batches, options);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  ArrowArrayStream stream;
  simdjson_arrow::make_stream(schema, std::move(batches), &stream);  // takes the batches
  auto reader = arrow::ImportRecordBatchReader(&stream);              // takes the stream
  if (!reader.ok()) {
    std::fprintf(stderr, "%s\n", reader.status().ToString().c_str());
    return EXIT_FAILURE;
  }
  auto table = (*reader)->ToTable();
  if (!table.ok()) {
    std::fprintf(stderr, "%s\n", table.status().ToString().c_str());
    return EXIT_FAILURE;
  }
  std::printf("%lld rows, %d columns\n%s\n", static_cast<long long>((*table)->num_rows()),
              (*table)->num_columns(), (*table)->schema()->ToString().c_str());

  // The arrow::Table back to NDJSON.
  ArrowArrayStream out;
  auto batch_reader = std::make_shared<arrow::TableBatchReader>(**table);
  if (!arrow::ExportRecordBatchReader(batch_reader, &out).ok()) return EXIT_FAILURE;
  std::string ndjson;
  simdjson_arrow::ndjson_write_options write_options;
  write_options.threads = options.threads;
  s = simdjson_arrow::write_ndjson(&out, &ndjson, write_options);  // releases the stream
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  std::printf("%zu bytes of JSON written; the first line:\n%s\n", ndjson.size(),
              ndjson.substr(0, ndjson.find('\n')).c_str());
  return EXIT_SUCCESS;
}

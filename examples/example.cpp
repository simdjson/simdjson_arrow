// Reads newline-delimited JSON into Arrow record batches, given a schema or
// inferring it, and writes the batches back as newline-delimited JSON.
//   c++ -O3 -std=c++17 example.cpp simdjson_arrow.cpp simdjson.cpp -o example
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "simdjson_arrow.h"

using namespace simdjson_arrow;

int main() {
  const std::string input =
      "{\"id\": 1, \"name\": \"apple\", \"tags\": [\"red\", \"round\"], \"price\": 1.25}\n"
      "{\"id\": 2, \"name\": \"pear\", \"tags\": [], \"extra\": true}\n"
      "{\"id\": 3, \"tags\": null, \"price\": 3}\n";
  simdjson::padded_string json(input);

  // With a schema: the keys absent from it ("extra") are skipped, the fields
  // absent from a record are null.
  field schema = make_schema({make_field("id", type::int64), make_field("name", type::utf8),
                              make_list("tags", make_field("item", type::utf8)),
                              make_field("price", type::float64)});
  std::vector<ArrowArray> batches;
  status s = read_ndjson(schema, json, &batches);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  int64_t rows = 0;
  for (const ArrowArray& b : batches) rows += b.length;
  std::printf("%lld rows in %zu batch(es)\n", static_cast<long long>(rows), batches.size());

  // Back to JSON: the writer takes the Arrow schema with the batches.
  ArrowSchema arrow_schema;
  if (!export_schema(schema, &arrow_schema).ok()) return EXIT_FAILURE;
  ndjson_writer writer;
  for (const ArrowArray& b : batches) {
    s = writer.write(&arrow_schema, &b);
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.message.c_str());
      return EXIT_FAILURE;
    }
  }
  std::printf("%.*s", static_cast<int>(writer.view().size()), writer.view().data());
  arrow_schema.release(&arrow_schema);
  for (ArrowArray& b : batches) b.release(&b);
  batches.clear();

  // Inferring the schema: "extra" becomes a boolean column, "price" a float64
  // column (an integer next to a float).
  field inferred;
  s = read_ndjson(json, &inferred, &batches);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  for (const field& f : inferred.children) {
    std::printf("%s: %s\n", f.name.c_str(),
                f.type == type::int64     ? "int64"
                : f.type == type::float64 ? "float64"
                : f.type == type::utf8    ? "utf8"
                : f.type == type::boolean ? "boolean"
                : f.type == type::list    ? "list"
                                          : "other");
  }
  for (ArrowArray& b : batches) b.release(&b);
  batches.clear();

  // Errors give the row and the field.
  simdjson::padded_string bad(std::string("{\"id\": 1}\n{\"id\": \"two\"}\n"));
  s = read_ndjson(schema, bad, &batches);
  std::printf("error: %s\n", s.message.c_str());
  return EXIT_SUCCESS;
}

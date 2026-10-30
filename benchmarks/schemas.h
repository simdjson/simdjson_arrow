// The schemas of the benchmark inputs.
// flat: the records of gendata.py in the blog benchmark (9 columns);
// gh: six fields of the GH Archive events (the other fields are skipped).
#ifndef SIMDJSON_ARROW_BENCHMARK_SCHEMAS_H
#define SIMDJSON_ARROW_BENCHMARK_SCHEMAS_H

#include <string>

#include "simdjson_arrow.h"

inline bool benchmark_schema(const std::string& name, simdjson_arrow::field* out) {
  using namespace simdjson_arrow;
  if (name == "flat") {
    *out = make_schema({make_field("id", type::int64), make_field("user", type::int64),
                        make_field("price", type::float64), make_field("lat", type::float64),
                        make_field("lon", type::float64), make_field("name", type::utf8),
                        make_field("tag", type::utf8), make_field("active", type::boolean),
                        make_field("count", type::int64)});
    return true;
  }
  if (name == "gh") {
    *out = make_schema(
        {make_field("id", type::utf8), make_field("type", type::utf8),
         make_struct("actor", {make_field("id", type::int64), make_field("login", type::utf8)}),
         make_struct("repo", {make_field("id", type::int64), make_field("name", type::utf8)}),
         make_field("public", type::boolean), make_field("created_at", type::utf8)});
    return true;
  }
  return false;
}

#endif  // SIMDJSON_ARROW_BENCHMARK_SCHEMAS_H

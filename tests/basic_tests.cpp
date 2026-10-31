// Tests without dependencies: we read the exported Arrow buffers directly.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/parallel.h"
#include "simdjson_arrow.h"

using namespace simdjson_arrow;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                       \
    }                                                                   \
  } while (0)

// Reads json, which must succeed, into batches.
static std::vector<ArrowArray> read(const field& schema, const std::string& json,
                                    ndjson_options options = {}) {
  simdjson::padded_string padded(json);
  std::vector<ArrowArray> batches;
  status s = read_ndjson(schema, padded, &batches, options);
  if (!s.ok()) {
    std::printf("unexpected error: %s\n", s.message.c_str());
    failures++;
  }
  return batches;
}

// Reads json, which must fail, and returns the error message.
static std::string read_error(const field& schema, const std::string& json,
                              ndjson_options options = {}) {
  simdjson::padded_string padded(json);
  std::vector<ArrowArray> batches;
  status s = read_ndjson(schema, padded, &batches, options);
  CHECK(!s.ok());
  CHECK(batches.empty());
  return s.message;
}

static void release(std::vector<ArrowArray>& batches) {
  for (ArrowArray& b : batches) b.release(&b);
}

static bool is_valid(const ArrowArray* a, int64_t i) {
  if (a->buffers[0] == nullptr) return true;
  return (static_cast<const uint8_t*>(a->buffers[0])[i >> 3] >> (i & 7)) & 1;
}

template <typename T>
static T value_at(const ArrowArray* a, int64_t i) {
  return static_cast<const T*>(a->buffers[1])[i];
}

static std::string string_at(const ArrowArray* a, int64_t i) {
  const int32_t* offsets = static_cast<const int32_t*>(a->buffers[1]);
  const char* chars = static_cast<const char*>(a->buffers[2]);
  return std::string(chars + offsets[i], static_cast<size_t>(offsets[i + 1] - offsets[i]));
}

static bool contains(const std::string& haystack, const char* needle) {
  bool found = haystack.find(needle) != std::string::npos;
  if (!found) std::printf("  message: %s\n", haystack.c_str());
  return found;
}

static void test_scalars() {
  field schema = make_schema({make_field("i", type::int64), make_field("u", type::uint64),
                              make_field("d", type::float64), make_field("s", type::utf8),
                              make_field("b", type::boolean), make_field("n", type::null)});
  auto batches = read(schema,
                      "{\"i\": -3, \"u\": 18446744073709551615, \"d\": 2.5, \"s\": \"h\\u00e9\", "
                      "\"b\": true, \"n\": null}\n"
                      "{\"i\": null, \"d\": 7, \"s\": \"\", \"b\": false}\n"
                      "{\"b\": null, \"s\": null, \"u\": 1, \"i\": 9223372036854775807}\n");
  CHECK(batches.size() == 1);
  const ArrowArray& t = batches[0];
  CHECK(t.length == 3);
  CHECK(t.n_children == 6);
  CHECK(t.null_count == 0);
  const ArrowArray* i = t.children[0];
  CHECK(value_at<int64_t>(i, 0) == -3);
  CHECK(!is_valid(i, 1));
  CHECK(i->null_count == 1);
  CHECK(value_at<int64_t>(i, 2) == INT64_MAX);
  const ArrowArray* u = t.children[1];
  CHECK(value_at<uint64_t>(u, 0) == UINT64_MAX);
  CHECK(!is_valid(u, 1));
  CHECK(value_at<uint64_t>(u, 2) == 1);
  const ArrowArray* d = t.children[2];
  CHECK(value_at<double>(d, 0) == 2.5);
  CHECK(value_at<double>(d, 1) == 7.0);
  CHECK(!is_valid(d, 2));
  const ArrowArray* s = t.children[3];
  CHECK(string_at(s, 0) == "h\xc3\xa9");
  CHECK(string_at(s, 1).empty() && is_valid(s, 1));
  CHECK(!is_valid(s, 2));
  const ArrowArray* b = t.children[4];
  CHECK(b->buffers[0] == nullptr || (is_valid(b, 0) && is_valid(b, 1)));
  CHECK((static_cast<const uint8_t*>(b->buffers[1])[0] & 3) == 1);
  CHECK(!is_valid(b, 2));
  const ArrowArray* n = t.children[5];
  CHECK(n->n_buffers == 0 && n->null_count == 3);
  release(batches);
}

static void test_nested() {
  field schema = make_schema(
      {make_struct("actor", {make_field("id", type::int64), make_field("login", type::utf8)}),
       make_list("tags", make_field("item", type::utf8)),
       make_list("matrix", make_list("item", make_field("item", type::int64)))});
  auto batches = read(schema,
                      "{\"actor\": {\"login\": \"bob\", \"id\": 7}, \"tags\": [\"a\", \"b\"], "
                      "\"matrix\": [[1], [], [2, 3]]}\n"
                      "{\"actor\": null, \"tags\": [], \"other\": {\"x\": [1, {\"y\": 2}]}}\n"
                      "{\"tags\": [null, \"c\"], \"matrix\": null}\n");
  CHECK(batches.size() == 1);
  const ArrowArray& t = batches[0];
  const ArrowArray* actor = t.children[0];
  CHECK(actor->length == 3 && actor->null_count == 2);
  CHECK(value_at<int64_t>(actor->children[0], 0) == 7);
  CHECK(string_at(actor->children[1], 0) == "bob");
  CHECK(actor->children[0]->length == 3);
  const ArrowArray* tags = t.children[1];
  const int32_t* offsets = static_cast<const int32_t*>(tags->buffers[1]);
  CHECK(offsets[0] == 0 && offsets[1] == 2 && offsets[2] == 2 && offsets[3] == 4);
  CHECK(string_at(tags->children[0], 1) == "b");
  CHECK(!is_valid(tags->children[0], 2));
  CHECK(string_at(tags->children[0], 3) == "c");
  const ArrowArray* matrix = t.children[2];
  CHECK(matrix->null_count == 2);
  const ArrowArray* rows = matrix->children[0];
  CHECK(rows->length == 3);
  CHECK(value_at<int64_t>(rows->children[0], 2) == 3);
  release(batches);
}

static void test_keys() {
  field schema = make_schema({make_field("a", type::int64), make_field("b\"c", type::int64)});
  // escaped keys, keys out of order, a key with a quote in the schema
  auto batches = read(schema, "{\"\\u0061\": 1, \"b\\\"c\": 2}\n{\"b\\\"c\": 3, \"a\": 4}\n");
  CHECK(batches.size() == 1);
  CHECK(value_at<int64_t>(batches[0].children[0], 0) == 1);
  CHECK(value_at<int64_t>(batches[0].children[1], 0) == 2);
  CHECK(value_at<int64_t>(batches[0].children[0], 1) == 4);
  CHECK(value_at<int64_t>(batches[0].children[1], 1) == 3);
  release(batches);
}

static void test_batches() {
  field schema = make_schema({make_field("a", type::int64)});
  std::string json;
  for (int i = 0; i < 1000; i++) json += "{\"a\": " + std::to_string(i) + "}\n";
  ndjson_options options;
  options.block_size = 1000;
  auto batches = read(schema, json, options);
  CHECK(batches.size() > 5);
  int64_t total = 0, expected = 0;
  bool in_order = true;
  for (ArrowArray& b : batches) {
    for (int64_t i = 0; i < b.length; i++) {
      in_order = in_order && value_at<int64_t>(b.children[0], i) == expected++;
    }
    total += b.length;
  }
  CHECK(total == 1000);
  CHECK(in_order);
  release(batches);
  auto none = read(schema, "  \n\n ");
  CHECK(none.empty());
}

static void test_errors() {
  field schema = make_schema({make_field("i", type::int64), make_field("s", type::utf8),
                              make_field("x", type::int64, /*nullable=*/false)});
  CHECK(contains(read_error(schema, "{\"x\": 1}\n{\"x\": 2, \"i\": \"no\"}\n"),
                 "expected a 64-bit signed integer, found a string (row 1, field /i)"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"i\": 1.5}"), "found 1.5"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"i\": 99999999999999999999}"),
                 "found 99999999999999999999"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"s\": 5}"), "expected a string, found a number"));
  CHECK(contains(read_error(schema, "{\"i\": 1}"), "missing non-nullable field"));
  CHECK(contains(read_error(schema, "{\"x\": null}"), "null in a non-nullable field"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"x\": 2}"), "duplicate key"));
  CHECK(contains(read_error(schema, "[1, 2]"), "a record must be a JSON object"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"i\": 01}"), "found 01"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"i\": tru}"), "invalid"));
  CHECK(!read_error(schema, "{\"x\": 1, \"i\": 2").empty());
  ndjson_options strict;
  strict.error_on_unknown_fields = true;
  CHECK(contains(read_error(schema, "{\"x\": 1, \"zz\": 2}", strict), "key absent from the schema"));
  // skipped values are validated, unless we ask otherwise
  CHECK(contains(read_error(schema, "{\"x\": 1, \"zz\": [1, tru]}"), "invalid"));
  CHECK(contains(read_error(schema, "{\"x\": 1, \"zz\": {\"a\": 01}}"), "invalid JSON number"));
  ndjson_options lax;
  lax.validate_skipped = false;
  auto batches = read(schema, "{\"x\": 1, \"zz\": {\"a\": 123456789012345678901234567890}}", lax);
  release(batches);
  batches = read(schema, "{\"x\": 1, \"zz\": 123456789012345678901234567890}");
  release(batches);
}

static void test_schema_round_trip() {
  field schema = make_schema(
      {make_field("i", type::int64, false),
       make_struct("o", {make_list("l", make_field("item", type::float64))})});
  ArrowSchema exported;
  CHECK(export_schema(schema, &exported).ok());
  CHECK(std::strcmp(exported.format, "+s") == 0);
  CHECK(std::strcmp(exported.children[0]->format, "l") == 0);
  CHECK(exported.children[0]->flags == 0);
  CHECK(std::strcmp(exported.children[1]->children[0]->format, "+l") == 0);
  field back;
  CHECK(import_schema(&exported, &back).ok());
  CHECK(back.children.size() == 2);
  CHECK(back.children[1].children[0].children[0].type == type::float64);
  CHECK(!back.children[0].nullable && back.children[1].nullable);
  exported.release(&exported);
}

static void test_write() {
  field schema = make_schema(
      {make_field("i", type::int64), make_field("d", type::float64), make_field("s", type::utf8),
       make_struct("o", {make_field("b", type::boolean), make_list("l", make_field("x", type::uint64))})});
  const std::string json =
      "{\"i\":-1,\"d\":0.1,\"s\":\"a\\\"\\n\\u0001\",\"o\":{\"b\":true,\"l\":[1,18446744073709551615]}}\n"
      "{\"i\":null,\"d\":1.0,\"s\":null,\"o\":null}\n"
      "{\"i\":3,\"d\":null,\"s\":\"\",\"o\":{\"b\":null,\"l\":[]}}\n";
  ndjson_options options;
  options.block_size = 64;  // several batches
  auto batches = read(schema, json, options);
  CHECK(batches.size() > 1);
  ArrowSchema exported;
  CHECK(export_schema(schema, &exported).ok());
  std::string out;
  for (ArrowArray& b : batches) CHECK(write_ndjson(&exported, &b, &out).ok());
  CHECK(out == json);
  ndjson_write_options no_nulls;
  no_nulls.write_nulls = false;
  std::string compact;
  ArrowArrayStream stream;
  make_stream(schema, std::move(batches), &stream);
  CHECK(write_ndjson(&stream, &compact, no_nulls).ok());
  CHECK(stream.release == nullptr);
  CHECK(compact.find("\n{\"d\":1.0}\n{\"i\":3,\"s\":\"\",\"o\":{\"l\":[]}}\n") != std::string::npos);
  exported.release(&exported);
}

// An allocator that counts its buffers.
struct counting_allocator {
  int live = 0;
  static char* reallocate(void* context, char* p, size_t, size_t size) {
    if (p == nullptr) static_cast<counting_allocator*>(context)->live++;
    return static_cast<char*>(std::realloc(p, size));
  }
  static void deallocate(void* context, char* p) {
    static_cast<counting_allocator*>(context)->live--;
    std::free(p);
  }
};

static void test_write_allocator() {
  field schema = make_schema({make_field("i", type::int64), make_field("s", type::utf8)});
  std::string json;
  for (int i = 0; i < 100000; i++) json += "{\"i\":" + std::to_string(i) + ",\"s\":\"x\"}\n";
  ArrowSchema exported;
  CHECK(export_schema(schema, &exported).ok());
  for (size_t threads : {1, 4}) {
    auto batches = read(schema, json);
    counting_allocator counter;
    ndjson_allocator allocator{counting_allocator::reallocate, counting_allocator::deallocate,
                               &counter};
    ndjson_write_options options;
    options.threads = threads;
    {
      ndjson_writer writer(options, allocator);
      for (ArrowArray& b : batches) CHECK(writer.write(&exported, &b).ok());
      CHECK(counter.live == 1);
      size_t size;
      char* p = writer.release(&size);
      CHECK(std::string_view(p, size) == json && p[size] == '\0');
      allocator.deallocate(&counter, p);
      CHECK(writer.write(&exported, &batches[0]).ok());  // a new buffer, freed by the writer
      CHECK(counter.live == 1);
    }
    CHECK(counter.live == 0);
    release(batches);
  }
  exported.release(&exported);
}

// An exception in fn, on a worker or on the calling thread, reaches the
// caller after all the threads are joined (it does not abort).
static void test_parallel_for() {
  for (size_t where : {size_t(0), size_t(37), size_t(999)}) {
    std::atomic<size_t> calls{0};
    bool caught = false;
    try {
      internal::parallel_for(1000, 4, [&](size_t i) {
        calls++;
        if (i == where) throw std::runtime_error("boom");
      });
    } catch (const std::runtime_error& e) {
      caught = std::string(e.what()) == "boom";
    }
    CHECK(caught);
    CHECK(calls.load() <= 1000);
  }
  std::vector<int> seen(1000, 0);
  internal::parallel_for(seen.size(), 8, [&](size_t i) { seen[i]++; });
  bool once = true;
  for (int x : seen) once = once && x == 1;
  CHECK(once);
}

// A failing allocator gives "out of memory", with one thread or several.
static void test_write_out_of_memory() {
  field schema = make_schema({make_field("i", type::int64)});
  std::string json;
  for (int i = 0; i < 100000; i++) json += "{\"i\":" + std::to_string(i) + "}\n";
  ArrowSchema exported;
  CHECK(export_schema(schema, &exported).ok());
  auto batches = read(schema, json);
  auto fail = [](void*, char*, size_t, size_t) -> char* { return nullptr; };
  auto drop = [](void*, char*) {};
  ndjson_allocator allocator{fail, drop, nullptr};
  for (size_t threads : {1, 4}) {
    ndjson_write_options options;
    options.threads = threads;
    ndjson_writer writer(options, allocator);
    status s = writer.write(&exported, &batches[0]);
    CHECK(!s.ok() && s.message == "out of memory");
    CHECK(writer.view().empty());
  }
  release(batches);
  exported.release(&exported);
}

// An allocator that fails at its call number fail_at, freeing p as it fails
// (as a Python bytes object does): the writer must forget p, report "out of
// memory", and stay usable.
struct failing_allocator {
  int calls = 0;
  int fail_at = 0;
  int live = 0;
  static char* reallocate(void* context, char* p, size_t, size_t size) {
    failing_allocator& a = *static_cast<failing_allocator*>(context);
    if (++a.calls == a.fail_at) {
      if (p != nullptr) a.live--;
      std::free(p);
      return nullptr;
    }
    if (p == nullptr) a.live++;
    return static_cast<char*>(std::realloc(p, size));
  }
  static void deallocate(void* context, char* p) {
    static_cast<failing_allocator*>(context)->live--;
    std::free(p);
  }
};

static void test_write_allocator_failures() {
  field schema = make_schema({make_field("i", type::int64), make_field("s", type::utf8)});
  std::string json;
  for (int i = 0; i < 20000; i++) json += "{\"i\":" + std::to_string(i) + ",\"s\":\"x\"}\n";
  ArrowSchema exported;
  CHECK(export_schema(schema, &exported).ok());
  auto batches = read(schema, json);
  std::vector<const ArrowArray*> pointers;
  for (ArrowArray& b : batches) pointers.push_back(&b);
  for (size_t threads : {1, 4}) {
    for (int fail_at = 1; fail_at <= 12; fail_at++) {
      failing_allocator a;
      a.fail_at = fail_at;
      ndjson_allocator allocator{failing_allocator::reallocate, failing_allocator::deallocate, &a};
      ndjson_write_options options;
      options.threads = threads;
      {
        ndjson_writer writer(options, allocator);
        // Batch by batch (the buffer grows with one thread), then all at once.
        status s;
        for (ArrowArray& b : batches) {
          if (!(s = writer.write(&exported, &b)).ok()) break;
        }
        if (s.ok()) s = writer.write(&exported, pointers.data(), pointers.size());
        CHECK(s.ok() ? writer.view() == json + json
                     : s.message == "out of memory" && writer.view().empty());
        a.fail_at = 0;
        CHECK(writer.write(&exported, pointers.data(), pointers.size()).ok());  // usable again
        CHECK(writer.view().substr(writer.view().size() - json.size()) == json);
      }
      CHECK(a.live == 0);
    }
  }
  release(batches);
  exported.release(&exported);
}

// Infers the schema with several threads, and checks that we get the same
// JSON back as with one thread.
static void test_infer_threads() {
  std::string json;
  for (int i = 0; i < 20000; i++) {
    json += "{\"i\":" + std::to_string(i);
    if (i % 3 == 0) json += ",\"d\":" + std::to_string(i) + (i % 2 ? ".5" : "");
    if (i % 5 == 0) json += ",\"o\":{\"s\":\"x" + std::to_string(i) + "\",\"l\":[null," + std::to_string(i) + "]}";
    if (i > 15000) json += ",\"late\":true";
    json += "}\n";
  }
  simdjson::padded_string padded(json);
  std::string reference;
  field reference_schema;
  for (size_t threads : {1, 2, 7, 32}) {
    ndjson_options options;
    options.block_size = 4096;
    options.threads = threads;
    field schema;
    std::vector<ArrowArray> batches;
    status s = read_ndjson(padded, &schema, &batches, options);
    CHECK(s.ok());
    CHECK(schema.children.size() == 4);
    CHECK(schema.children[1].type == type::float64);
    CHECK(schema.children[2].children[1].children[0].type == type::int64);
    ArrowSchema exported;
    CHECK(export_schema(schema, &exported).ok());
    ndjson_write_options write_options;
    write_options.threads = threads;
    ndjson_writer writer(write_options);
    std::vector<const ArrowArray*> pointers;
    for (ArrowArray& b : batches) pointers.push_back(&b);
    CHECK(writer.write(&exported, pointers.data(), pointers.size()).ok());
    exported.release(&exported);
    release(batches);
    if (threads == 1) {
      reference = std::string(writer.view());
    } else {
      CHECK(writer.view() == reference);
    }
  }
  CHECK(reference.find("{\"i\":3,\"d\":3.5,\"o\":null,\"late\":null}") != std::string::npos);
}

// Without padding, we must not read past the end of the input (AddressSanitizer
// checks it: the input is allocated to its exact size).
static void test_unpadded() {
  std::string json;
  for (int i = 0; i < 3000; i++) json += "{\"a\":" + std::to_string(i) + ",\"s\":\"x\"}\n";
  json += "{\"a\":3000,\"s\":\"last line without a line end, ending with a number:\",\"n\":12345";
  json += "}";
  const std::string bad = json + "\n{\"a\":tru}";
  const std::string spanning = "{\"a\":\n1}\n{\"a\":\n2}";
  for (size_t threads : {1, 4}) {
    for (size_t block : {100, 4096, 1 << 20}) {
      ndjson_options options;
      options.threads = threads;
      options.block_size = block;
      const std::string* inputs[] = {&json, &bad, &spanning};
      for (const std::string* input : inputs) {
        std::unique_ptr<char[]> exact(new char[input->size()]);
        std::memcpy(exact.get(), input->data(), input->size());
        field schema;
        std::vector<ArrowArray> batches;
        status s = read_ndjson(exact.get(), input->size(), &schema, &batches, options);
        CHECK(s.ok() == (input != &bad));
        int64_t rows = 0;
        for (ArrowArray& b : batches) rows += b.length;
        CHECK(rows == (input == &json ? 3001 : input == &spanning ? 2 : 0));
        if (input == &bad) CHECK(contains(s.message, "row 3001"));
        release(batches);
      }
    }
  }
}

int main() {
  test_parallel_for();
  test_write_out_of_memory();
  test_write_allocator_failures();
  test_unpadded();
  test_infer_threads();
  test_write();
  test_write_allocator();
  test_scalars();
  test_nested();
  test_keys();
  test_batches();
  test_errors();
  test_schema_round_trip();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all tests passed\n");
  return EXIT_SUCCESS;
}

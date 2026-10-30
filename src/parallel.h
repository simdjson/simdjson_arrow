// Internal: running work on several threads.
#ifndef SIMDJSON_ARROW_PARALLEL_H
#define SIMDJSON_ARROW_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace simdjson_arrow {
namespace internal {

// Calls fn(i) for i in [0, n), with up to `threads` threads (the calling
// thread is one of them). Each thread takes the next i from a shared counter.
template <typename Fn>
void parallel_for(size_t n, size_t threads, Fn fn) {
  threads = std::min(threads, n);
  if (threads <= 1) {
    for (size_t i = 0; i < n; i++) fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i = next++; i < n; i = next++) fn(i);
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < threads; t++) pool.emplace_back(work);
  work();
  for (std::thread& t : pool) t.join();
}

}  // namespace internal
}  // namespace simdjson_arrow

#endif  // SIMDJSON_ARROW_PARALLEL_H

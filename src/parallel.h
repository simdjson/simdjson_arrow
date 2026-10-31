// Internal: running work on several threads.
#ifndef SIMDJSON_ARROW_PARALLEL_H
#define SIMDJSON_ARROW_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace simdjson_arrow {
namespace internal {

// Calls fn(i) for i in [0, n), with up to `threads` threads (the calling
// thread is one of them). Each thread takes the next i from a shared counter.
// If fn throws, the threads stop taking work, we join them all and rethrow
// the first exception on the calling thread. If a thread cannot be started,
// the threads we have do the work.
template <typename Fn>
void parallel_for(size_t n, size_t threads, Fn fn) {
  threads = std::min(threads, n);
  if (threads <= 1) {
    for (size_t i = 0; i < n; i++) fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::mutex mutex;
  std::exception_ptr error;
  auto work = [&] {
    try {
      for (size_t i = next++; i < n; i = next++) fn(i);
    } catch (...) {
      next = n;  // the others stop
      std::lock_guard<std::mutex> lock(mutex);
      if (!error) error = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  try {
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; t++) pool.emplace_back(work);
  } catch (...) {  // std::system_error or std::bad_alloc: fewer threads
  }
  work();
  for (std::thread& t : pool) t.join();
  if (error) std::rethrow_exception(error);
}

}  // namespace internal
}  // namespace simdjson_arrow

#endif  // SIMDJSON_ARROW_PARALLEL_H

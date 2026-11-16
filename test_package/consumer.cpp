#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
int main() {
  const std::size_t n = 100000;
  auto values = parlay::tabulate(n, [](std::size_t i) { return static_cast<std::uint64_t>(n - i); });
  if (parlay::reduce(values) != std::uint64_t{n} * (n + 1) / 2) return 1;
  auto sorted = parlay::sort(values);
  for (std::size_t i = 0; i < n; ++i)
    if (sorted[i] != i + 1) return 2;
  std::atomic<std::size_t> visited{0};
  parlay::parallel_for(0, n, [&](std::size_t) { visited.fetch_add(1, std::memory_order_relaxed); });
  return visited.load() == n ? 0 : 3;
}

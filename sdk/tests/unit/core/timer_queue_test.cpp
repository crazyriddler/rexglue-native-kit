/**
 * @file        tests/unit/core/timer_queue_test.cpp
 * @brief       Kit test: rex::thread TimerQueue timing and idle behaviour.
 *
 * The TimerQueue waits with disruptorplus::blocking_wait_strategy. The kit fixed the
 * argument order of its wait_for/wait_until calls (sdk/patches/thirdparty/
 * 0001-disruptorplus-wait-arg-order.patch, EXP-040: the spinning strategy cost ~18% of a
 * core while idle). These cases check that timers fire on time and that an idle queue
 * sleeps instead of spinning.
 */

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/thread/timer_queue.h>

using namespace std::chrono_literals;
using Clock = rex::thread::TimerQueueWaitItem::clock;

TEST_CASE("TimerQueue fires a one-shot timer at its due time", "[timer_queue]") {
  std::atomic<bool> fired{false};
  std::atomic<int64_t> fired_at_ns{0};
  auto start = Clock::now();
  auto item = rex::thread::QueueTimerOnce(
      [&](void*) {
        fired_at_ns = (Clock::now() - start).count();
        fired = true;
      },
      nullptr, start + 30ms);
  for (int i = 0; i < 200 && !fired; ++i) std::this_thread::sleep_for(5ms);
  REQUIRE(fired);
  auto elapsed = std::chrono::nanoseconds(fired_at_ns.load());
  CHECK(elapsed >= 29ms);
  CHECK(elapsed < 500ms);
}

TEST_CASE("TimerQueue recurring timer repeats until disarmed", "[timer_queue]") {
  std::atomic<int> count{0};
  auto item = rex::thread::QueueTimerRecurring([&](void*) { ++count; }, nullptr,
                                               Clock::now() + 5ms, 10ms);
  std::this_thread::sleep_for(200ms);
  if (auto locked = item.lock()) locked->Disarm();
  int at_disarm = count.load();
  CHECK(at_disarm >= 5);
  std::this_thread::sleep_for(60ms);
  CHECK(count.load() == at_disarm);
}

#if !defined(_WIN32)
// std::clock() is process CPU time on POSIX (wall time on Windows, so skipped there).
TEST_CASE("TimerQueue does not spin while waiting", "[timer_queue]") {
  std::atomic<bool> fired{false};
  // Make sure the queue thread exists, then leave it waiting on a far due time.
  rex::thread::QueueTimerOnce([&](void*) {}, nullptr, Clock::now());
  std::this_thread::sleep_for(20ms);
  auto item = rex::thread::QueueTimerOnce([&](void*) { fired = true; }, nullptr,
                                          Clock::now() + 600ms);
  std::clock_t cpu0 = std::clock();
  std::this_thread::sleep_for(500ms);
  double cpu_ms = 1000.0 * double(std::clock() - cpu0) / CLOCKS_PER_SEC;
  CHECK_FALSE(fired);
  INFO("idle process CPU over 500 ms: " << cpu_ms << " ms");
  // Measured on Linux (clang 20): blocking strategy ~0.1 ms, upstream spin strategy ~12.5 ms
  // (it yields and sleeps between spins; on Windows the same loop cost ~18% of a core).
  CHECK(cpu_ms < 3.0);
  for (int i = 0; i < 100 && !fired; ++i) std::this_thread::sleep_for(10ms);
  CHECK(fired);
}
#endif

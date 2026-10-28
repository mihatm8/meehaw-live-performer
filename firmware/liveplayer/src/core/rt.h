// meehaw live performer — lock-free single-producer single-consumer queue and atomics (SPEC §7).
#pragma once
#include <stdint.h>
#include <atomic>
#include "config.h"
#include "types.h"

namespace lp {

template <typename T, int N>
class SpscQueue {
 public:
  bool push(const T& v) {
    uint32_t h = head_.load(std::memory_order_relaxed);
    uint32_t n = (h + 1) % N;
    if (n == tail_.load(std::memory_order_acquire)) return false;
    buf_[h] = v;
    head_.store(n, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = buf_[t];
    tail_.store((t + 1) % N, std::memory_order_release);
    return true;
  }
  int size() const { int d = (int)head_.load() - (int)tail_.load(); return d < 0 ? d + N : d; }
 private:
  T buf_[N];
  std::atomic<uint32_t> head_{0}, tail_{0};
};

using CmdQueue = SpscQueue<Cmd, kCmdQueueSize>;

// Engine -> UI event ring (note triggers for the perform page metronome, bar lines, errors).
enum EvType : uint8_t { EvNone = 0, EvBar, EvBeat, EvTrackStarted, EvTrackStopped, EvUnderrun, EvKick };
struct Ev { EvType type = EvNone; uint8_t a = 0; uint32_t tick = 0; };
using EvQueue = SpscQueue<Ev, 64>;

} // namespace lp

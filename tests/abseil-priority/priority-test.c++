// Exercise the real mutex enqueue path with success, sandbox denials, and an
// unrelated error. Each test runs in its own process to reset per-thread state.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>

#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace {
int errorCode;
int calls;
bool never(void*) { return false; }
}

extern "C" int __wrap_pthread_getschedparam(pthread_t, int* policy,
                                           struct sched_param* param) {
  ++calls;
  if (errorCode != 0) return errorCode;
  *policy = SCHED_OTHER;
  param->sched_priority = 0;
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  errorCode = std::atoi(argv[1]);
  absl::Mutex mutex;
  mutex.Lock();
  for (int i = 0; i < 20; ++i) {
    if (mutex.AwaitWithTimeout(absl::Condition(&never, nullptr),
                               absl::Milliseconds(1))) return 3;
  }
  mutex.Unlock();
  // Success is cached; sandbox denials disable further probes. Other errors
  // retain Abseil's existing diagnostic/retry behavior.
  bool expected = errorCode == ESRCH ? calls >= 20 : calls == 1;
  if (!expected) {
    std::fprintf(stderr, "error=%d: unexpected probe count %d\n", errorCode, calls);
    return 1;
  }
}

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace daichod {

// Thrown to the calling gRPC thread when the bounded queue is full or the
// worker is draining; mapped to RESOURCE_EXHAUSTED / UNAVAILABLE upstairs.
struct QueueFullError : std::runtime_error {
  QueueFullError() : std::runtime_error("engine queue full") {}
};
struct WorkerStoppedError : std::runtime_error {
  WorkerStoppedError() : std::runtime_error("engine worker is draining") {}
};

// The single engine thread. libgnucash is not thread-safe and assumes one
// writer; every engine touch — reads included — goes through Run() and
// executes strictly in FIFO order on one dedicated thread. There is no
// engine-side locking because there is no engine-side concurrency.
class EngineWorker {
 public:
  explicit EngineWorker(std::size_t max_queue_depth);
  ~EngineWorker();

  EngineWorker(const EngineWorker&) = delete;
  EngineWorker& operator=(const EngineWorker&) = delete;

  // Blocks the calling thread until fn has executed on the engine thread and
  // returns its result; exceptions thrown by fn propagate to the caller.
  template <typename F>
  auto Run(F&& fn) -> std::invoke_result_t<F> {
    using R = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
    auto future = task->get_future();
    Enqueue([task] { (*task)(); });
    return future.get();
  }

  std::size_t queue_depth() const;

  // Stops accepting new work, executes everything already queued, joins.
  // Idempotent; called on SIGTERM.
  void Drain();

 private:
  void Enqueue(std::function<void()> task);
  void Loop();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  const std::size_t max_depth_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace daichod

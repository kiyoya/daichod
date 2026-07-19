#include "engine/worker.h"

namespace daichod {

EngineWorker::EngineWorker(std::size_t max_queue_depth)
    : max_depth_(max_queue_depth), thread_([this] { Loop(); }) {}

EngineWorker::~EngineWorker() { Drain(); }

void EngineWorker::Enqueue(std::function<void()> task) {
  {
    std::lock_guard lock(mu_);
    if (stopping_) throw WorkerStoppedError{};
    if (queue_.size() >= max_depth_) throw QueueFullError{};
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

std::size_t EngineWorker::queue_depth() const {
  std::lock_guard lock(mu_);
  return queue_.size();
}

void EngineWorker::Drain() {
  {
    std::lock_guard lock(mu_);
    if (stopping_ && !thread_.joinable()) return;
    stopping_ = true;
  }
  cv_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void EngineWorker::Loop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping and fully drained
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task();  // packaged_task: exceptions land in the caller's future
  }
}

}  // namespace daichod

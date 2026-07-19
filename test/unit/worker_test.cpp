#include "engine/worker.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace daichod {
namespace {

// Helper that occupies the engine thread with a task blocked on a promise,
// so subsequent Run()s from the test observably sit in the queue rather than
// racing to completion. started_future fires once the blocking task is
// actually executing on the engine thread (i.e. the queue has drained past
// it); the test releases the block by setting release_promise.
struct BlockingGate {
  std::promise<void> started_promise;
  std::promise<void> release_promise;
  std::shared_future<void> release_future = release_promise.get_future().share();

  // Submits the blocking task on a caller-owned thread (since Run() blocks
  // the calling thread until the task completes) and waits until the task
  // has actually started executing on the engine thread.
  std::thread Occupy(EngineWorker* worker) {
    std::thread blocker([this, worker] {
      worker->Run([this] {
        started_promise.set_value();
        release_future.wait();
        return 0;
      });
    });
    started_promise.get_future().wait();
    return blocker;
  }

  void Release() { release_promise.set_value(); }
};

TEST(EngineWorkerTest, RunsTasksInFifoOrder) {
  EngineWorker worker(8);
  std::vector<int> order;
  std::mutex order_mu;

  for (int i = 0; i < 5; ++i) {
    worker.Run([&order, &order_mu, i] {
      std::lock_guard<std::mutex> lock(order_mu);
      order.push_back(i);
      return 0;
    });
  }

  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(EngineWorkerTest, ReturnValuePropagates) {
  EngineWorker worker(8);
  int result = worker.Run([] { return 42; });
  EXPECT_EQ(result, 42);

  std::string text = worker.Run([] { return std::string("hello"); });
  EXPECT_EQ(text, "hello");
}

TEST(EngineWorkerTest, ExceptionPropagatesToCaller) {
  EngineWorker worker(8);
  EXPECT_THROW(
      worker.Run([]() -> int { throw std::runtime_error("boom"); }),
      std::runtime_error);

  // The worker must remain usable after a task throws.
  EXPECT_EQ(worker.Run([] { return 7; }), 7);
}

TEST(EngineWorkerTest, QueueFullErrorWhenDepthExceeded) {
  constexpr std::size_t kMaxDepth = 2;
  EngineWorker worker(kMaxDepth);

  BlockingGate gate;
  std::thread blocker = gate.Occupy(&worker);

  // The engine thread is now stuck inside the blocking task; queue_ is
  // empty (the task was already popped). Fill it to max_depth_.
  std::vector<std::thread> fillers;
  for (std::size_t i = 0; i < kMaxDepth; ++i) {
    fillers.emplace_back([&worker] { worker.Run([] { return 0; }); });
  }
  while (worker.queue_depth() < kMaxDepth) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_THROW(worker.Run([] { return 0; }), QueueFullError);

  gate.Release();
  blocker.join();
  for (auto& t : fillers) t.join();
}

TEST(EngineWorkerTest, RunAfterDrainThrowsWorkerStoppedError) {
  EngineWorker worker(8);
  worker.Drain();
  EXPECT_THROW(worker.Run([] { return 0; }), WorkerStoppedError);

  // Drain is idempotent.
  worker.Drain();
}

TEST(EngineWorkerTest, DrainExecutesAlreadyQueuedWork) {
  EngineWorker worker(8);
  std::atomic<int> counter{0};

  // BlockingGate::Occupy runs a bare task; wrap it so the blocked task also
  // increments counter once released, so we can verify it ran.
  std::promise<void> started_promise;
  std::promise<void> release_promise;
  std::shared_future<void> release_future = release_promise.get_future().share();
  std::thread blocker([&worker, &started_promise, release_future, &counter] {
    worker.Run([&started_promise, release_future, &counter] {
      started_promise.set_value();
      release_future.wait();
      counter.fetch_add(1);
      return 0;
    });
  });
  started_promise.get_future().wait();

  std::thread queued_task([&worker, &counter] {
    worker.Run([&counter] {
      counter.fetch_add(1);
      return 0;
    });
  });
  while (worker.queue_depth() < 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::thread drainer([&worker] { worker.Drain(); });
  // Give Drain time to set stopping_ and start waiting on the join before
  // releasing the blocked task.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  release_promise.set_value();

  drainer.join();
  blocker.join();
  queued_task.join();

  EXPECT_EQ(counter.load(), 2);
  EXPECT_THROW(worker.Run([] { return 0; }), WorkerStoppedError);
}

TEST(EngineWorkerTest, QueueDepthReportsPendingCount) {
  EngineWorker worker(8);
  EXPECT_EQ(worker.queue_depth(), 0u);

  BlockingGate gate;
  std::thread blocker = gate.Occupy(&worker);
  // The blocking task is executing, not queued.
  EXPECT_EQ(worker.queue_depth(), 0u);

  std::thread queued_task([&worker] { worker.Run([] { return 0; }); });
  while (worker.queue_depth() < 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(worker.queue_depth(), 1u);

  gate.Release();
  blocker.join();
  queued_task.join();
  EXPECT_EQ(worker.queue_depth(), 0u);
}

}  // namespace
}  // namespace daichod

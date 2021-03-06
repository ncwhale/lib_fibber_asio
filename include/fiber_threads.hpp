//
// fiber_threads.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Whale Mo (ncwhale at gmail dot com)
//
#ifndef ASIO_FIBER_THREAD_HPP
#define ASIO_FIBER_THREAD_HPP

#include <boost/fiber/all.hpp>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include "thread_barrier.hpp"
#include "thread_name.hpp"

namespace asio_fiber {

typedef std::function<void()> task_type;

template <typename fiber_scheduling_algorithm =
              boost::fibers::algo::shared_work,
          std::size_t fiber_group_id = 128>
class FiberThreads {
 public:
  static FiberThreads &instance();

  void init(std::size_t count = 2, bool use_this_thread = true,
            bool suspend_worker_thread = true);

  void post(task_type);

  void notify_stop();

  void join();

 private:
  FiberThreads() = default;
  FiberThreads(const FiberThreads &rhs) = delete;
  FiberThreads(FiberThreads &&rhs) = delete;

  FiberThreads &operator=(const FiberThreads &rhs) = delete;
  FiberThreads &operator=(FiberThreads &&rhs) = delete;

  bool running = false;
  std::size_t fiber_thread_count;
  std::mutex run_mtx;
  boost::fibers::condition_variable_any m_cnd_stop;
  std::vector<std::thread> m_threads;
  boost::fibers::unbuffered_channel<task_type> task_channel;
};

template <typename fiber_scheduling_algorithm>
void install_fiber_scheduling_algorithm(std::size_t thread_count,
                                        bool suspend) {
  // Default scheduling need zero param.
  boost::fibers::use_scheduling_algorithm<fiber_scheduling_algorithm>();
}

template <>
void install_fiber_scheduling_algorithm<boost::fibers::algo::shared_work>(
    std::size_t thread_count, bool suspend) {
  boost::fibers::use_scheduling_algorithm<boost::fibers::algo::shared_work>(
      suspend);
}

template <>
void install_fiber_scheduling_algorithm<boost::fibers::algo::work_stealing>(
    std::size_t thread_count, bool suspend) {
  boost::fibers::use_scheduling_algorithm<boost::fibers::algo::work_stealing>(
      thread_count, suspend);
}

template <typename fiber_scheduling_algorithm,
          std::size_t fiber_group_id>
FiberThreads<fiber_scheduling_algorithm, fiber_group_id> &
FiberThreads<fiber_scheduling_algorithm, fiber_group_id>::instance() {
  static FiberThreads<fiber_scheduling_algorithm, fiber_group_id> ft;
  return ft;
}

template <typename fiber_scheduling_algorithm,
          std::size_t fiber_group_id>
void FiberThreads<fiber_scheduling_algorithm, fiber_group_id>::init(
    std::size_t count, bool use_this_thread, bool suspend_worker_thread) {
  // Check param for init.
  if (!use_this_thread and count < 1) {
    // TODO: throw expection?
    return;
  }

  {  // Only init when not running.
    std::lock_guard<std::mutex> lk(run_mtx);
    if (running) return;
    running = true;
    fiber_thread_count = count;
  }

  // This will start a work post fiber on every thread.
  auto install_task_post_fiber = [this] {
    auto worker_fiber = boost::fibers::fiber([this] {
      task_type task;
      // dequeue & process tasks.
      while (boost::fibers::channel_op_status::closed !=
             task_channel.pop(task)) {
        task();
      }
    });

    // auto context = worker_fiber.properties();
    worker_fiber.detach();
  };

  // At least we need 2 threads for other fiber algo.
  if (use_this_thread && fiber_thread_count < 2) {
    // Use round_robin for this (main) thread only.
    install_fiber_scheduling_algorithm<boost::fibers::algo::round_robin>(
        fiber_thread_count, suspend_worker_thread);

    // Install task post fiber.
    install_task_post_fiber();

    return;
  }

  thread_barrier b(fiber_thread_count);

  for (std::size_t i = (use_this_thread ? 1 : 0); i < fiber_thread_count; ++i) {
    m_threads.push_back(std::thread(
        [&b, i, this, suspend_worker_thread, &install_task_post_fiber] {
          {
            std::ostringstream oss;
            oss << "Fiber-Thread-" << i;
            this_thread_name::set(oss.str());
          }

          install_fiber_scheduling_algorithm<fiber_scheduling_algorithm>(
              fiber_thread_count, suspend_worker_thread);

          // Sync all threads.
          b.wait();

          // Install task post fiber.
          install_task_post_fiber();

          {  // Wait for fibers run.
            std::unique_lock<std::mutex> lk(run_mtx);
            m_cnd_stop.wait(lk, [this]() { return !running; });
          }
        }));
  }

  if (use_this_thread) {
    install_fiber_scheduling_algorithm<fiber_scheduling_algorithm>(
        fiber_thread_count, suspend_worker_thread);
    // sync with worker threads.
    b.wait();

    // Install task post fiber.
    install_task_post_fiber();
  }
}

template <typename fiber_scheduling_algorithm,
          std::size_t fiber_group_id>
void FiberThreads<fiber_scheduling_algorithm, fiber_group_id>::post(
    task_type task) {
  task_channel.push(task);
}

template <typename fiber_scheduling_algorithm,
          std::size_t fiber_group_id>
void FiberThreads<fiber_scheduling_algorithm,
                  fiber_group_id>::notify_stop() {
  std::unique_lock<std::mutex> lk(run_mtx);
  running = false;
  lk.unlock();
  m_cnd_stop.notify_all();
}

template <typename fiber_scheduling_algorithm,
          std::size_t fiber_group_id>
void FiberThreads<fiber_scheduling_algorithm, fiber_group_id>::join() {
  //检查结束条件
  {
    std::unique_lock<std::mutex> lk(run_mtx);
    m_cnd_stop.wait(lk, [this]() { return !running; });
  }

  for (std::thread &t : m_threads) {
    if (t.joinable()) t.join();
  }
}

}  // namespace asio_fiber

#endif  // ASIO_FIBER_THREAD_HPP

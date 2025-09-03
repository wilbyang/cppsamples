#include <iostream>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <deque>
#include <random>
#include <chrono>
#include <atomic>

class WorkStealingQueue {
public:
    void push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_front(std::move(task));
    }

    bool pop(std::function<void()>& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool steal(std::function<void()>& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.back());
        queue_.pop_back();
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

class ThreadPool {
public:
    ThreadPool(size_t num_threads) : queues_(num_threads), done_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this, i] {
                worker(i);
            });
        }
    }

    ~ThreadPool() {
        done_ = true;
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

    void submit(std::function<void()> task) {
        // 简单地提交到某个线程的队列（可以轮询或随机）
        static std::atomic<size_t> index{0};
        size_t i = index++;
        queues_[i % queues_.size()].push(std::move(task));
    }

private:
    void worker(size_t index) {
        while (!done_) {
            std::function<void()> task;
            bool found = false;

            // 先从自己的队列中取任务
            if (queues_[index].pop(task)) {
                found = true;
            } else {
                // 从其他线程中偷任务
                for (size_t i = 0; i < queues_.size(); ++i) {
                    if (queues_[(index + i) % queues_.size()].steal(task)) {
                        found = true;
                        break;
                    }
                }
            }

            if (found) {
                task();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    std::vector<std::thread> threads_;
    std::vector<WorkStealingQueue> queues_;
    std::atomic<bool> done_;
};

// 示例任务函数
void example_task(int id) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100 + id % 5 * 10));
    std::cout << "Task " << id << " executed by thread " << std::this_thread::get_id() << std::endl;
}

int main() {
    ThreadPool pool(4);

    for (int i = 0; i < 20; ++i) {
        pool.submit([i] { example_task(i); });
    }

    std::this_thread::sleep_for(std::chrono::seconds(3)); // 等待任务完成
    return 0;
}
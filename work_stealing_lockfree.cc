#include <iostream>
#include <thread>
#include <vector>
#include <functional>
#include <random>
#include <chrono>
#include <atomic>
#include <boost/lockfree/queue.hpp>

// 基础版无锁任务队列
class LockFreeTaskQueue {
public:
    LockFreeTaskQueue(size_t capacity = 1024) {
        (void)capacity; // fixed capacity via template parameter
    }
    
    void push(std::function<void()> task) {
        // allocate the task on heap so the pointer is trivially destructible
        auto* task_ptr = new std::function<void()>(std::move(task));
        while (!queue_.push(task_ptr)) {
            std::this_thread::yield();
        }
    }
    
    bool pop(std::function<void()>& task) {
        std::function<void()>* ptr = nullptr;
        if (queue_.pop(ptr) && ptr) {
            task = std::move(*ptr);
            delete ptr;
            return true;
        }
        return false;
    }
    
    bool empty() const {
        return queue_.empty();
    }

    ~LockFreeTaskQueue() {
        // Drain any remaining tasks to avoid leaks
        std::function<void()>* ptr = nullptr;
        while (queue_.pop(ptr)) {
            delete ptr;
        }
    }

private:
    // Use pointer type because Boost lockfree queue requires trivially destructible T
    boost::lockfree::queue<std::function<void()>*, boost::lockfree::capacity<1024>> queue_;
};

// 进阶版无锁任务队列 - 支持 work-stealing
class AdvancedLockFreeTaskQueue {
public:
    AdvancedLockFreeTaskQueue(size_t capacity = 1024) {
        (void)capacity; // fixed capacity via template parameter
    }
    
    void push(std::function<void()> task) {
        auto* task_ptr = new std::function<void()>(std::move(task));
        while (!local_queue_.push(task_ptr)) {
            std::this_thread::yield();
        }
    }
    
    bool pop(std::function<void()>& task) {
        // 优先从本地队列取
        {
            std::function<void()>* ptr = nullptr;
            if (local_queue_.pop(ptr) && ptr) {
                task = std::move(*ptr);
                delete ptr;
                return true;
            }
        }
        // 然后从可被偷取的队列取
        {
            std::function<void()>* ptr = nullptr;
            if (steal_queue_.pop(ptr) && ptr) {
                task = std::move(*ptr);
                delete ptr;
                return true;
            }
        }
        return false;
    }
    
    // 专门用于偷取任务的接口
    bool steal(std::function<void()>& task) {
        std::function<void()>* ptr = nullptr;
        if (steal_queue_.pop(ptr) && ptr) {
            task = std::move(*ptr);
            delete ptr;
            return true;
        }
        return false;
    }
    
    // 将本地任务转移到可被偷取的队列
    void publish_tasks() {
        // move some tasks from local_queue_ to steal_queue_
        // 转移一半任务到steal_queue
        size_t local_size = 0;
        // 粗略估计队列大小
        std::function<void()>* ptr = nullptr;
        while (local_size < 5 && local_queue_.pop(ptr)) {
            // forward the pointer to the steal queue
            while (!steal_queue_.push(ptr)) {
                std::this_thread::yield();
            }
            ++local_size;
        }
    }
    
    bool empty() const {
        return local_queue_.empty() && steal_queue_.empty();
    }

    ~AdvancedLockFreeTaskQueue() {
        // Drain remaining tasks in both queues
        std::function<void()>* ptr = nullptr;
        while (local_queue_.pop(ptr)) {
            delete ptr;
        }
        while (steal_queue_.pop(ptr)) {
            delete ptr;
        }
    }

private:
    // 本地队列 - 线程优先使用
    boost::lockfree::queue<std::function<void()>*, boost::lockfree::capacity<1024>> local_queue_;
    // 可被偷取的队列 - 其他线程可以访问
    boost::lockfree::queue<std::function<void()>*, boost::lockfree::capacity<1024>> steal_queue_;
};

// 使用基础版的简单线程池
class SimpleLockFreeThreadPool {
public:
    explicit SimpleLockFreeThreadPool(size_t num_threads = std::thread::hardware_concurrency()) 
        : queues_(num_threads), done_(false), num_threads_(num_threads) {
        
        for (size_t i = 0; i < num_threads_; ++i) {
            threads_.emplace_back([this, i] {
                simple_worker(i);
            });
        }
    }
    
    ~SimpleLockFreeThreadPool() {
        done_ = true;
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    void submit(std::function<void()> task) {
        static std::atomic<size_t> index{0};
        size_t i = index++;
        queues_[i % num_threads_].push(std::move(task));
    }

private:
    void simple_worker(size_t thread_id) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<size_t> dist(0, num_threads_ - 1);
        
        while (!done_) {
            std::function<void()> task;
            bool found = false;
            
            // 1. 尝试从自己的队列获取任务
            if (queues_[thread_id].pop(task)) {
                found = true;
            }
            // 2. 尝试从其他队列偷取任务
            else {
                size_t victim = dist(rng);
                if (victim != thread_id && queues_[victim].pop(task)) {
                    found = true;
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
    std::vector<LockFreeTaskQueue> queues_;  // 使用基础版队列
    std::atomic<bool> done_;
    size_t num_threads_;
};

// 使用进阶版的高性能线程池
class AdvancedLockFreeThreadPool {
public:
    explicit AdvancedLockFreeThreadPool(size_t num_threads = std::thread::hardware_concurrency()) 
        : queues_(num_threads), done_(false), num_threads_(num_threads) {
        
        std::random_device rd;
        rng_.seed(rd());
        
        // 创建工作线程
        for (size_t i = 0; i < num_threads_; ++i) {
            threads_.emplace_back([this, i] {
                advanced_worker(i);
            });
        }
        
        // 创建任务发布线程
        publisher_thread_ = std::thread([this] {
            task_publisher();
        });
    }
    
    ~AdvancedLockFreeThreadPool() {
        shutdown();
    }
    
    void submit(std::function<void()> task) {
        static std::atomic<size_t> index{0};
        size_t i = index++;
        queues_[i % num_threads_].push(std::move(task));
    }
    
    void wait_empty() {
        while (!all_queues_empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    void advanced_worker(size_t thread_id) {
        std::uniform_int_distribution<size_t> dist(0, num_threads_ - 1);
        
        while (!done_) {
            std::function<void()> task;
            bool executed = false;
            
            // 1. 尝试从自己的队列获取任务
            if (queues_[thread_id].pop(task)) {
                task();
                executed = true;
            }
            // 2. 尝试偷取其他线程的任务
            else {
                size_t victim = dist(rng_);
                if (victim != thread_id && queues_[victim].steal(task)) {
                    task();
                    executed = true;
                }
            }
            
            if (!executed) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }
    
    void task_publisher() {
        while (!done_) {
            for (auto& queue : queues_) {
                queue.publish_tasks();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    void shutdown() {
        done_ = true;
        if (publisher_thread_.joinable()) {
            publisher_thread_.join();
        }
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    bool all_queues_empty() const {
        for (const auto& queue : queues_) {
            if (!queue.empty()) {
                return false;
            }
        }
        return true;
    }
    
    std::vector<std::thread> threads_;
    std::thread publisher_thread_;
    std::vector<AdvancedLockFreeTaskQueue> queues_;  // 使用进阶版队列
    std::atomic<bool> done_;
    size_t num_threads_;
    std::mt19937 rng_;
};

// 测试函数
void test_simple_pool() {
    std::cout << "=== Simple Lock-Free Thread Pool ===" << std::endl;
    SimpleLockFreeThreadPool pool(4);
    
    for (int i = 0; i < 10; ++i) {
        pool.submit([i] {
            std::cout << "Task " << i << " executed by thread " 
                     << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

void test_advanced_pool() {
    std::cout << "\n=== Advanced Lock-Free Thread Pool ===" << std::endl;
    AdvancedLockFreeThreadPool pool(4);
    
    for (int i = 0; i < 10; ++i) {
        pool.submit([i] {
            std::cout << "Advanced Task " << i << " executed by thread " 
                     << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    }
    
    pool.wait_empty();
    std::cout << "All advanced tasks completed!" << std::endl;
}

int main() {
    test_simple_pool();
    test_advanced_pool();
    return 0;
}
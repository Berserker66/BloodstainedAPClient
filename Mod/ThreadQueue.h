#pragma once
#include <functional>
#include <mutex>
#include <queue>

class ThreadQueue {
   public:
    static ThreadQueue& Instance() {
        static ThreadQueue instance;
        return instance;
    }

    void Enqueue(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(fn);
    }

    void Flush() {
        std::queue<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(queue);
        }
        while (!pending.empty()) {
            pending.front()();
            pending.pop();
        }
    }

   private:
    std::queue<std::function<void()>> queue;
    std::mutex mutex;
};

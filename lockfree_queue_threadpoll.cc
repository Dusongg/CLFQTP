#ifndef __dusong_lock_free_thread_pool_H__
#define __dusong_lock_free_thread_pool_H__

#include <memory>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace DusongPool {
    template<typename T>
    class LockFreeQueue {
    private:
        struct Node
        {
            std::shared_ptr<T> data;   //可以不用shared_ptr,直接用指针
            Node* next;
            Node() : next(nullptr) {}
        };
        std::atomic<Node*> head;
        std::atomic<Node*> tail;   //不存放数据，指向下一个要插入的节点的位置

    public:
        LockFreeQueue() : head(new Node()), tail(head) {}

        void push(T& value) {
            std::shared_ptr<T> newdata(std::make_shared(std::move(value)));
            Node* newnode = new Node;
            Node* cur_tail = nullptr;
            Node* prev_tail = nullptr;
            while (true) {
                cur_tail = tail.load(std::memory_order_acquire);   //读
                prev_tail = tail.exchange(newnode, std::memory_order_acq_rel);
                //防止prev_tail被其他线程修改
                if (cur_tail == prev_tail) break;
                else {
                    //还原
                    tail.store(prev_tail, std::memory_order_release);
                }
            }
            prev_tail.data.swap(newdata);   //等价于prev_tail.data.reset(newdata);
            prev_tail->next.stroe(newnode, std::memory_order_release);
        }

        std::shared_ptr<T> pop() {
            Node* cur_head = nullptr;
            while (true) {
                cur_head = head.load(std::memory_order_relaxed);
                if (cur_head == tail.laod(std::memory_order_acquire)) {   //队列里没有数据
                    return std::shared_ptr<T>();
                }
                Node* new_head = cur_head->next.load(std::memory_order_acquire);
                /*
                bool compare_exchange_weak(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) noexcept;
                如果当前对象的值等于 `expected`，则将当前对象的值修改为 `desired`，并返回 `true`；
                否则，将 `expected` 更新为当前对象的值，并返回 `false`。这个操作是原子的，能够在多线程环境下确保操作的原子性。
                */
                if (head.compare_exchange_weak(cur_head, new_head)) {
                    std::shared_ptr<T> ret(std::make_shared(cur_head->data));
                    delete cur_head;
                    return ret;
                }
            }
        }

        bool pop(T& output) {
            Node* cur_head = nullptr;
            while (true) {
                cur_head = head.load(std::memory_order_relaxed);
                if (cur_head == tail.laod(std::memory_order_acquire)) {   //队列里没有数据
                    return false;
                }
                Node* new_head = cur_head->next.load(std::memory_order_acquire);
                if (head.compare_exchange_weak(cur_head, new_head)) {
                    T = cur_head->data.get();
                    delete cur_head;
                    return true;
                }
            }
        }

        bool empty() {
            //应该可以用relaxed
            Node* cur_head = head.load(std::memory_order_relaxed);
            Node* cur_tail = tail.load(std::memory_order_relaxed);
            return cur_head == cur_tail;
        }
    };

    class thread_pool {
    private:
        std::vector<std::unique_ptr<std::thread>> threads;
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;  //初始化false表示可以使用, true表示该线程想要停止
        LockFreeQueue<std::function<void(int id)>*> lock_free_queue;
        std::atomic<bool> is_done;
        std::atomic<bool> is_stop;
        std::atomic<int> n_waiting;

        std::mutex mutex;
        std::condition_variable cond_v;

    public:
        thread_pool() { this->init(); }
        thread_pool(int pool_size) {
            this->init();
            this->resize(pool_size);
        }
        thread_pool(const thread_pool&) = delete;
        thread_pool(thread_pool&&) = delete;
        thread_pool& operator=(const thread_pool&) = delete;
        thread_pool& operator=(thread_pool&&) = delete;
        ~thread_pool() { this->stop(true); }

        int size() { return static_cast<int>(this->threads.size()); }

        void resize(int newsize) {
            if (!this->is_stop && !this->is_done) {
                int oldsize = static_cast<int>(this->threads.size());
                if (oldsize < newsize) {
                    this->threads.resize(newsize);
                    this->flags.resize(newsize);
                    for (int i = oldsize; i < newsize; i++) {
                        this->flags[i] = std::make_shared<std::atomic<bool>>(false);
                        this->set_thread(i);
                    }
                } else if (oldsize > newsize) {
                    for (int i = newsize; i < oldsize; i++) {
                        *this->flags[i] = true;
                        this->threads[i]->detach();
                    }
                    {
                        std::lock_guard<std::mutex> lock(this->mutex);
                        this->cond_v.notify_all();  //通知所有被阻塞的线程执行任务
                    }
                    this->threads.resize(newsize);
                    this->flags.resize(newsize);
                }
            }
        }

        auto pop() -> std::function<void(int)> {
            std::function<void(int)>* task = nullptr;
            this->lock_free_queue.pop(task);
            std::unique_ptr<std::function<void(int)>> func(task);    //异常退出时，delete function
            std::function<void(int)> f;
            if (task != nullptr) f = *task;
            return f;
        }

        // wait for all computing threads to finish and stop all threads
        // may be called asynchronously to not pause the calling thread while waiting
        // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
        void stop(bool is_wait = false) {
            
        }

        //清空队列里的所有任务
        void clear_queue() {
            std::function<void(int id)>* f;
            while (this->lock_free_queue.pop(f)) { delete f; }
        }



    private:
        void init() { n_waiting = 0; is_done = false; is_stop = false; }

        //创建并运行线程
        //id表示threads[i]的线程，建立一个task与id的映射
        void set_thread(int id) {
            std::shared_ptr<std::atomic<bool>> flag(this->flags[id]);

            this->threads[id].reset(new std::thread([this, id, flag /*值传递*/]() {
                std::atomic<bool>& _flag = *flag;   //可省略？
                std::function<void(int)>* task;
                bool is_pop = this->lock_free_queue.pop(task);

                //线程执行
                while (true) {
                    while (is_pop) {
                        std::unique_ptr<std::function<void(int)>> func(task);   //释放任务,出作用域时
                        (*task)(id);
                        if (_flag) { return; } else { is_pop = this->lock_free_queue.pop(task); }
                    }
                    //unique_lock与lock_guard的区别：unique_lock在后续代码中可以多次上锁和解锁，而lock_guard不能，这里条件变量必须用unqiue_lock
                    std::unique_lock<std::mutex> lock(this->mutex);
                    this->n_waiting++;
                    this->cond_v.wait(lock, [this, &task, &is_pop, &_flag]() {
                        is_pop = this->lock_free_queue.pop(task);
                        return is_pop || this->is_done || _flag;   //唤醒线程当且仅当:取出任务/线程执行完毕/第id个线程被暂停
                    });
                    this->n_waiting--;
                    if (!is_pop) return;  //线程执行完毕/第id个线程被暂停，此时线程结束运行，return
                }
            }
            ));
        }
    };
}

#endif
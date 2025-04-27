#include <iostream>
#include <queue>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <functional>
#include <chrono>
#include <random>
#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace std;
using namespace chrono;
using task_func = function<void()>;

// Thread-local worker identifier
thread_local int tls_worker_id = 0;

// Global synchronization and metrics
mutex cout_mutex;   // Protects console output
mutex metrics_mutex;    // Protects metric containers
vector<size_t> global_queue_samples;    // Stores queue lengths over time
vector<int> global_task_durations;  // Stores task execution durations

class thread_pool {
public:
    thread_pool() : stop_flag_(false), paused_flag_(false) {}
    ~thread_pool() { stop_immediate(); }

    // Start the pool with the given number of workers
    void start(size_t num_workers) {
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back(&thread_pool::worker_routine, this, int(i + 1));
        }
    }

    // Submit a new task to the queue
    void submit(task_func task) {
        {
            lock_guard<mutex> lk(queue_mutex_);
            if (stop_flag_) return;
            tasks_.push(move(task));
        }
        queue_cv_.notify_one();
    }

    // Pause processing new tasks
    void pause() {
        paused_flag_.store(true);
    }

    // Resume processing tasks
    void resume() {
        paused_flag_.store(false);
        queue_cv_.notify_all();
    }

    // Graceful shutdown: finish queued and running tasks
    void stop_graceful() {
        {
            lock_guard<mutex> lk(queue_mutex_);
            stop_flag_ = true;
        }
        queue_cv_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
        workers_.clear();
    }

    // Immediate shutdown: drop pending tasks and stop workers
    void stop_immediate() {
        {
            lock_guard<mutex> lk(queue_mutex_);
            stop_flag_ = true;
            queue<task_func> empty;
            tasks_.swap(empty); // Clear pending tasks
        }
        queue_cv_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
        workers_.clear();
    }

private:
    // Worker thread routine: fetch and execute tasks
    void worker_routine(int worker_id) {
        tls_worker_id = worker_id;
        while (true) {
            task_func task;
            {
                unique_lock<mutex> lk(queue_mutex_);
                queue_cv_.wait(lk, [this] {
                    return stop_flag_ || (!paused_flag_.load() && !tasks_.empty());
                });
                if (stop_flag_ && tasks_.empty()) {
                    break;
                }
                if (!paused_flag_.load() && !tasks_.empty()) {
                    task = move(tasks_.front());
                    tasks_.pop();
                }
            }
            task();
        }
    }

    vector<thread> workers_;
    queue<task_func> tasks_;
    mutex queue_mutex_;
    condition_variable queue_cv_;
    bool stop_flag_;
    atomic<bool> paused_flag_;
};

// Scheduler with double buffering and 60s intervals
class scheduler {
public:
    scheduler(thread_pool& pool)
        : pool_(pool), running_(false), current_buffer_(0), task_counter_(0)
    {
        buffers_[0] = {};
        buffers_[1] = {};
    }

    // Start the scheduling thread
    void start() {
        running_ = true;
        sched_thread_ = thread(&scheduler::schedule_loop, this);
    }

    // Stop scheduling
    void stop() {
        running_ = false;
        sched_thread_.join();
    }

    // Add a new task with specified work time
    void add_task(int work_time) {
        int id = ++task_counter_;
        lock_guard<mutex> lk(buffer_mutex_);
        int buf = current_buffer_;
        {
            lock_guard<mutex> out(cout_mutex);
            cout << "Task " << id << " added to buffer " << (buf + 1) << endl;
        }
        buffers_[buf].push({ id, work_time, buf });
    }

private:
    struct task_item { int id, work_time, buffer_id; };

    // Main loop: fill buffer for 60s, then execute
    void schedule_loop() {
        {
            lock_guard<mutex> out(cout_mutex);
            cout << "First 60 seconds used to fill the buffer..." << endl;
        }
        while (running_) {
            auto fill_start = high_resolution_clock::now();
            while (duration_cast<seconds>(high_resolution_clock::now() - fill_start).count() < 60) {
                // Sample queue length
                size_t len;
                {
                    lock_guard<mutex> buf_lock(buffer_mutex_);
                    len = buffers_[current_buffer_].size();
                }
                {
                    lock_guard<mutex> mlk(metrics_mutex);
                    global_queue_samples.push_back(len);
                }
                this_thread::sleep_for(seconds(1));
            }
            // Swap buffers
            queue<task_item> exec_queue;
            int exec_buf;
            {
                lock_guard<mutex> lk(buffer_mutex_);
                exec_buf = current_buffer_;
                current_buffer_ = 1 - current_buffer_;
                {
                    lock_guard<mutex> out(cout_mutex);
                    cout << "====================================" << endl;
                    cout << "Buffer switched: now active buffer " << (current_buffer_ + 1) << endl;
                    cout << "====================================" << endl;
                }
                exec_queue = move(buffers_[exec_buf]);
                buffers_[exec_buf] = {};
            }
            // Execute tasks from the swapped-out buffer
            while (!exec_queue.empty()) {
                auto item = exec_queue.front();
                exec_queue.pop();
                pool_.submit([item]() {
                    auto start = high_resolution_clock::now();
                    {
                        lock_guard<mutex> out(cout_mutex);
                        cout << "Task " << item.id
                            << " started (" << item.work_time << "s)"
                            << " from buffer " << (item.buffer_id + 1)
                            << " on worker " << tls_worker_id << endl;
                    }
                    this_thread::sleep_for(chrono::seconds(item.work_time));
                    auto end = high_resolution_clock::now();
                    auto dur = chrono::duration_cast<chrono::seconds>(end - start).count();
                    {
                        lock_guard<mutex> mlk(metrics_mutex);
                        global_task_durations.push_back(int(dur));
                    }
                    {
                        lock_guard<mutex> out(cout_mutex);
                        cout << "Task " << item.id
                            << " finished by worker " << tls_worker_id
                            << " in " << dur << "s" << endl;
                    }
                    });
            }
        }
    }

    thread_pool& pool_;
    atomic<bool> running_;
    thread sched_thread_;
    mutex buffer_mutex_;
    queue<task_item> buffers_[2];
    int current_buffer_;
    atomic<int> task_counter_;
};

int main() {
    // Initialize random generators for producers
    random_device rd;
    mt19937 rng(rd());
    uniform_int_distribution<int> sleep_dist(3, 6);
    uniform_int_distribution<int> work_dist(6, 15);

    const int num_workers = 6;
    // Initialize the thread pool with num_workers
    thread_pool pool;
    pool.start(num_workers);

    // Initialize and start the scheduler
    scheduler sched(pool);
    sched.start();

    // Start producer threads
    atomic<bool> producing(true);
    vector<thread> producers;
    for (int i = 0; i < 3; ++i) {
        producers.emplace_back([&] {
            while (producing) {
                int wt = work_dist(rng);
                sched.add_task(wt);
                this_thread::sleep_for(chrono::seconds(sleep_dist(rng)));
            }
        });
    }

    // Example pause/resume usage
    this_thread::sleep_for(chrono::seconds(80));
    pool.pause();
    {
        lock_guard<mutex> out(cout_mutex);
        cout << "====================================" << endl;
        cout << "Thread pool paused for 20 seconds..." << endl;
        cout << "====================================" << endl;
    }
    this_thread::sleep_for(chrono::seconds(20));
    pool.resume();
    {
        lock_guard<mutex> out(cout_mutex);
        cout << "====================================" << endl;
        cout << "Thread pool resumed." << endl;
        cout << "====================================" << endl;
    }

    this_thread::sleep_for(chrono::seconds(20));
    producing = false;
    for (auto& producer : producers) {
        producer.join();
    }

    // Stop scheduler and thread pool gracefully
    sched.stop();
    pool.stop_graceful();

    // Print summary metrics
    long long sum_dur = 0;
    size_t count_dur = 0;
    {
        lock_guard<mutex> mlk(metrics_mutex);
        for (auto d : global_task_durations) sum_dur += d;
        count_dur = global_task_durations.size();
    }
    double avg_dur = count_dur ? double(sum_dur) / count_dur : 0.0;

    long long sum_q = 0;
    size_t count_q = 0;
    {
        lock_guard<mutex> mlk(metrics_mutex);
        for (auto q : global_queue_samples) sum_q += q;
        count_q = global_queue_samples.size();
    }
    double avg_q = count_q ? double(sum_q) / count_q : 0.0;

    {
        lock_guard<mutex> out(cout_mutex);
        cout << "\n=== Summary ===" << endl;
        cout << "Threads created: " << num_workers << endl;
        cout << "Average queue length: " << avg_q << endl;
        cout << "Average task execution time: " << avg_dur << "s" << endl;
    }

    return 0;
}

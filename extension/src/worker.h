/* Copyright 2026 ETF2L Contributors, Dave
 *
 * A worker: one thread that runs the queued tasks one after the other and
 * keeps what each returned until the main thread collects it. */
#ifndef SMGIT_WORKER_H
#define SMGIT_WORKER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace smgit {

/* What a task hands back: run by the main thread, it delivers the task's
 * outcome. */
using Completion = std::function<void()>;

/* Runs on the worker's thread. abort turns true when the worker stops; a
 * long task should then end early. A task should not throw: one that does
 * is noted in the server log and hands nothing back. */
using Task = std::function<Completion(const std::atomic<bool> &abort)>;

class Worker {
public:
	Worker();
	/* Stops like Stop(). */
	~Worker();
	Worker(const Worker &) = delete;
	Worker &operator=(const Worker &) = delete;

	/* Queues the task behind all others. */
	void Submit(Task task);

	/* The completions of the tasks finished since the last call, in the
	 * order the tasks ran. */
	std::vector<Completion> TakeCompletions();

	/* Aborts the running task, drops the queued ones and the completions
	 * and joins the thread. Nothing is run after it returns. */
	void Stop();

private:
	void Run();
	/* The task's completion; an empty one when it threw. */
	Completion RunSafely(const Task &task) noexcept;

	std::mutex m_mutex;
	std::condition_variable m_wakeUp;
	std::deque<Task> m_queue;
	std::vector<Completion> m_completions;
	bool m_isStopping = false;
	std::atomic<bool> m_abort{false};
	std::thread m_thread;
};

} // namespace smgit

#endif // SMGIT_WORKER_H

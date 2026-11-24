/* Copyright 2026 ETF2L Contributors, Dave */
#include "worker.h"

#include "serverlog.h"

#include <exception>
#include <utility>

namespace smgit {

Worker::Worker() : m_thread(&Worker::Run, this)
{
}

Worker::~Worker()
{
	Stop();
}

void Worker::Submit(Task task)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_isStopping)
			return;
		m_queue.push_back(std::move(task));
	}
	m_wakeUp.notify_one();
}

std::vector<Completion> Worker::TakeCompletions()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return std::exchange(m_completions, {});
}

void Worker::Stop()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_isStopping = true;
		m_queue.clear();
		m_completions.clear();
	}
	m_abort = true;
	m_wakeUp.notify_one();
	if (m_thread.joinable())
		m_thread.join();
}

void Worker::Run()
{
	for (;;) {
		Task next;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_wakeUp.wait(lock, [this] { return m_isStopping || !m_queue.empty(); });
			if (m_isStopping)
				return;
			next = std::move(m_queue.front());
			m_queue.pop_front();
		}

		Completion completion = RunSafely(next);

		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_isStopping)
			return;
		if (!completion)
			continue;
		try {
			m_completions.push_back(std::move(completion));
		} catch (...) {
			NoteForLog(LogLevel::Error, "a finished task was dropped: out of memory");
		}
	}
}

Completion Worker::RunSafely(const Task &task) noexcept
{
	/* Tasks turn their own failures into results for the plugin; an
	 * exception that still gets here would otherwise end the game server. */
	try {
		return task(m_abort);
	} catch (const std::exception &failure) {
		NoteForLog(LogLevel::Error, "a task ended with an exception and no answer: ", failure.what());
	} catch (...) {
		NoteForLog(LogLevel::Error, "a task ended with an unknown exception and no answer");
	}
	return {};
}

} // namespace smgit

/* Copyright 2026 ETF2L Contributors, Dave */
#include "serverlog.h"

#include <mutex>
#include <utility>

namespace smgit {

namespace {

std::mutex g_mutex;
std::vector<LogLine> g_lines;

} // namespace

void NoteForLog(LogLevel level, const char *text, const char *detail) noexcept
{
	try {
		LogLine line{level, std::string(text) + detail};
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lines.push_back(std::move(line));
	} catch (...) {
		/* Out of memory: there is nowhere left to tell it. */
	}
}

std::vector<LogLine> TakeLogLines()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return std::exchange(g_lines, {});
}

} // namespace smgit

/* Copyright 2026 ETF2L Contributors, Dave
 *
 * Lines for the game server's log from any thread. Work deep in the core
 * that has something to tell without failing (a stale lock file deleted, a
 * task that threw) notes it here; the extension writes what was noted on
 * the game thread, where SourceMod's log may be used. */
#ifndef SMGIT_SERVERLOG_H
#define SMGIT_SERVERLOG_H

#include <string>
#include <vector>

namespace smgit {

enum class LogLevel { Message, Error };

struct LogLine {
	LogLevel level;
	std::string text;
};

/* Notes the line text followed by detail. Never throws, so it may be
 * called from a catch block; a line that cannot be kept for want of memory
 * is lost. */
void NoteForLog(LogLevel level, const char *text, const char *detail = "") noexcept;

/* The lines noted since the last call, oldest first. */
std::vector<LogLine> TakeLogLines();

} // namespace smgit

#endif // SMGIT_SERVERLOG_H

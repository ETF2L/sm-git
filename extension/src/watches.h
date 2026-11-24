/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The watches of one server: which watcher (a plugin, opaque here) watches
 * which ref of which repository at which interval, when each repository is
 * due to be fetched, and which watched refs moved since they were last
 * seen. The time is always passed in, so every rule can be tested without
 * waiting; nothing here fetches or knows SourceMod. */
#ifndef SMGIT_WATCHES_H
#define SMGIT_WATCHES_H

#include "remote.h"
#include "repo.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace smgit {

/* Who watches. The schedule only tells watchers apart. */
using Watcher = const void *;

using WatchClock = std::chrono::steady_clock;

/* A watched ref that is at another commit than when it was last seen; an
 * empty SHA is a ref that is not there. */
struct RefMove {
	std::string url;
	std::string ref;
	std::string oldSha;
	std::string newSha;
};

/* A repository whose watches are due to be fetched. */
struct DuePoll {
	std::string repoName;
	std::string path;
};

/* The commit a watched ref is at in a snapshot; empty when it is not
 * there. A name that is a branch and a tag is the branch: watches follow
 * what moves. (A revision of the same name resolves to the tag, as in
 * git.) */
std::string ShaOfWatchedRef(const RefSnapshot &snapshot, const std::string &ref);

class WatchSchedule {
public:
	/* configuredInterval is what a watch of interval 0 gets (the setting
	 * watch_interval, at least kMinWatchInterval). */
	explicit WatchSchedule(int configuredInterval);

	/* Has the watcher watch ref (smgit::IsWatchableRef) of the repository
	 * of remote at path. interval 0 is the configured one; intervals below
	 * kMinWatchInterval are raised to it. A ref nobody watched yet makes the
	 * repository due at once, so that the fetch notes where it is; a ref
	 * watched already keeps the repository's timer, which only comes sooner
	 * when this interval is shorter than what is left of it. */
	void Watch(const Remote &remote, const std::string &path, const std::string &ref, Watcher watcher, int interval,
	           WatchClock::time_point now);
	void Unwatch(const std::string &repoName, const std::string &ref, Watcher watcher);
	/* Ends every watch of the watcher. */
	void Forget(Watcher watcher);

	/* The repositories due at now that are not being fetched; each is being
	 * fetched from then until FinishPoll. */
	std::vector<DuePoll> TakeDue(WatchClock::time_point now);
	/* Ends the fetch TakeDue started: the repository is due again after the
	 * shortest interval of its watches. The refs on disk count also after a
	 * failed fetch, since another server sharing the root may have
	 * fetched. Returns the moves to tell. */
	std::vector<RefMove> FinishPoll(const std::string &repoName, const Result<FetchReport> &fetched,
	                                WatchClock::time_point now);
	/* Compares the repository's watched refs with refs and notes them as
	 * seen. A ref seen for the first time only is noted. Returns the moves
	 * to tell. */
	std::vector<RefMove> Observe(const std::string &repoName, const RefSnapshot &refs);

	bool IsWatched(const std::string &repoName) const;
	/* Every watcher of any ref of the repository, each once. */
	std::vector<Watcher> WatchersOf(const std::string &repoName) const;
	/* The repository's watched refs with their intervals, as
	 * "main (300 s), v1 (60 s)"; empty when it has none. */
	std::string Describe(const std::string &repoName) const;

private:
	struct RefWatch {
		std::map<Watcher, int> intervals; /* per watcher, never empty */
		std::string lastSha;
		bool isSeeded = false;
	};

	struct RepoWatch {
		std::string url;
		std::string path;
		std::map<std::string, RefWatch> refs; /* never empty */
		WatchClock::time_point nextPoll;      /* the clock's epoch: due at once */
		bool isPolling = false;
	};

	int IntervalOf(int requested) const;
	static int ShortestInterval(const RefWatch &watch);
	static int PollIntervalOf(const RepoWatch &watch);

	int m_configuredInterval;
	std::map<std::string, RepoWatch> m_repos; /* by repository name */
};

} // namespace smgit

#endif // SMGIT_WATCHES_H

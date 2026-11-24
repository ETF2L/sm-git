/* Copyright 2026 ETF2L Contributors, Dave */
#include "watches.h"

#include "config.h"

#include <limits.h>

#include <algorithm>
#include <set>
#include <utility>

namespace smgit {

std::string ShaOfWatchedRef(const RefSnapshot &snapshot, const std::string &ref)
{
	if (ref == kHeadRef)
		return snapshot.headSha;
	for (bool wantTag : {false, true}) {
		for (const Ref &candidate : snapshot.refs) {
			if (candidate.isTag == wantTag && candidate.name == ref)
				return candidate.sha;
		}
	}
	return {};
}

WatchSchedule::WatchSchedule(int configuredInterval) : m_configuredInterval(configuredInterval)
{
}

int WatchSchedule::IntervalOf(int requested) const
{
	return requested == 0 ? m_configuredInterval : std::max(requested, kMinWatchInterval);
}

void WatchSchedule::Watch(const Remote &remote, const std::string &path, const std::string &ref, Watcher watcher,
                          int interval, WatchClock::time_point now)
{
	int wanted = IntervalOf(interval);
	RepoWatch &repo = m_repos[remote.name];
	repo.url = remote.url;
	repo.path = path;
	bool isNewRef = repo.refs.count(ref) == 0;
	repo.refs[ref].intervals[watcher] = wanted;
	if (isNewRef)
		repo.nextPoll = WatchClock::time_point();
	else
		repo.nextPoll = std::min(repo.nextPoll, now + std::chrono::seconds(wanted));
}

void WatchSchedule::Unwatch(const std::string &repoName, const std::string &ref, Watcher watcher)
{
	auto repo = m_repos.find(repoName);
	if (repo == m_repos.end())
		return;
	auto watch = repo->second.refs.find(ref);
	if (watch == repo->second.refs.end())
		return;

	watch->second.intervals.erase(watcher);
	if (watch->second.intervals.empty())
		repo->second.refs.erase(watch);
	if (repo->second.refs.empty())
		m_repos.erase(repo);
}

void WatchSchedule::Forget(Watcher watcher)
{
	for (auto repo = m_repos.begin(); repo != m_repos.end();) {
		auto &refs = repo->second.refs;
		for (auto ref = refs.begin(); ref != refs.end();) {
			ref->second.intervals.erase(watcher);
			ref = ref->second.intervals.empty() ? refs.erase(ref) : std::next(ref);
		}
		repo = refs.empty() ? m_repos.erase(repo) : std::next(repo);
	}
}

std::vector<DuePoll> WatchSchedule::TakeDue(WatchClock::time_point now)
{
	std::vector<DuePoll> due;
	for (auto &[repoName, repo] : m_repos) {
		if (repo.isPolling || repo.nextPoll > now)
			continue;
		repo.isPolling = true;
		due.push_back(DuePoll{repoName, repo.path});
	}
	return due;
}

std::vector<RefMove> WatchSchedule::FinishPoll(const std::string &repoName, const Result<FetchReport> &fetched,
                                               WatchClock::time_point now)
{
	auto found = m_repos.find(repoName);
	if (found == m_repos.end())
		return {};
	found->second.isPolling = false;
	found->second.nextPoll = now + std::chrono::seconds(PollIntervalOf(found->second));
	if (!fetched.value.hasRefs)
		return {};
	return Observe(repoName, fetched.value.refs);
}

std::vector<RefMove> WatchSchedule::Observe(const std::string &repoName, const RefSnapshot &refs)
{
	auto found = m_repos.find(repoName);
	if (found == m_repos.end())
		return {};

	std::vector<RefMove> moves;
	for (auto &[ref, watch] : found->second.refs) {
		std::string sha = ShaOfWatchedRef(refs, ref);
		if (watch.isSeeded && sha != watch.lastSha)
			moves.push_back(RefMove{found->second.url, ref, watch.lastSha, sha});
		watch.lastSha = sha;
		watch.isSeeded = true;
	}
	return moves;
}

bool WatchSchedule::IsWatched(const std::string &repoName) const
{
	return m_repos.count(repoName) != 0;
}

std::vector<Watcher> WatchSchedule::WatchersOf(const std::string &repoName) const
{
	auto found = m_repos.find(repoName);
	if (found == m_repos.end())
		return {};

	std::set<Watcher> watchers;
	for (const auto &[ref, watch] : found->second.refs) {
		for (const auto &[watcher, wanted] : watch.intervals)
			watchers.insert(watcher);
	}
	return std::vector<Watcher>(watchers.begin(), watchers.end());
}

std::string WatchSchedule::Describe(const std::string &repoName) const
{
	auto found = m_repos.find(repoName);
	if (found == m_repos.end())
		return {};

	std::string described;
	for (const auto &[ref, watch] : found->second.refs)
		described += (described.empty() ? "" : ", ") + ref + " (" + std::to_string(ShortestInterval(watch)) + " s)";
	return described;
}

/* The shortest interval the watchers of the ref asked for. Each one already
 * has the configured interval in place of 0, so a longer interval than
 * watch_interval stands. */
int WatchSchedule::ShortestInterval(const RefWatch &watch)
{
	int shortest = INT_MAX;
	for (const auto &[watcher, wanted] : watch.intervals)
		shortest = std::min(shortest, wanted);
	return shortest;
}

/* The repository is fetched as often as its most often watched ref wants. */
int WatchSchedule::PollIntervalOf(const RepoWatch &repo)
{
	int shortest = INT_MAX;
	for (const auto &[ref, watch] : repo.refs)
		shortest = std::min(shortest, ShortestInterval(watch));
	return shortest;
}

} // namespace smgit

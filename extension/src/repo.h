/* Copyright 2026 ETF2L Contributors, Dave
 *
 * Everything the extension does with git, on bare repositories below one
 * root directory. Each function opens the repository it works on, does one
 * thing and closes it again, so it may run on any thread and sees what
 * another server sharing the root wrote in the meantime. None of them knows
 * SourceMod. */
#ifndef SMGIT_REPO_H
#define SMGIT_REPO_H

#include "config.h"
#include "remote.h"
#include "root.h"
#include "status.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace smgit {

/* What work() returns, a Result; when it throws, a Failed result that
 * tells why, so that the plugin waiting for it still gets an answer. */
template <typename Work>
auto Guarded(Work &&work) -> decltype(work())
{
	using Answer = decltype(work());
	try {
		return work();
	} catch (const std::exception &failure) {
		return Answer{Status::Failed, std::string("internal error: ") + failure.what(), {}};
	} catch (...) {
		return Answer{Status::Failed, "internal error: an unknown exception", {}};
	}
}

struct Ref {
	std::string name; /* short: "main", "v1.0" */
	std::string sha;  /* the commit it points to, tags peeled */
	bool isTag;
};

/* The branches and tags of a repository at one moment. */
struct RefSnapshot {
	std::string headBranch; /* the default branch, empty in an empty repository */
	std::string headSha;
	std::vector<Ref> refs;
};

struct FetchReport {
	bool changed = false;  /* a branch or tag moved, appeared or went away */
	bool hasRefs = false;  /* whether refs was read: always, unless the repository is gone */
	RefSnapshot refs;      /* as on disk after the fetch, also after a failed one */
};

struct Commit {
	std::string sha;
	std::string authorName;
	std::string authorEmail;
	int64_t time; /* author time, Unix seconds */
	std::string summary;
	std::string message;
};

enum class DiffStatus { Added = 0, Modified, Deleted };

struct DiffEntry {
	std::string path;
	DiffStatus status;
	std::string oldSha; /* blob in the base, empty when added */
	std::string newSha; /* blob in the head, empty when deleted */
};

enum class EntryType { File = 0, Directory, Symlink, Submodule };

struct TreeEntry {
	std::string path; /* from the repository's top */
	EntryType type;
	uint64_t size; /* of files and symlinks, otherwise 0 */
	std::string sha;
	bool isExecutable;
};

enum class ChangeKind { Written = 0, Deleted };

/* What a write-out did to one file. */
struct Change {
	std::string path; /* the destination, as the caller named it */
	ChangeKind kind;
};

enum class FileState { Same = 0, Different, Missing };

/* Where a write-out goes. */
struct Destination {
	std::string absolute;
	std::string display; /* the same place as the caller named it, for the Change list */
};

/* What a write-out must not do. Both directories are canonical (see
 * CanonicalPath), and every destination is resolved the same way before it
 * is compared with them, so a symlink cannot lead around them. */
struct WritePolicy {
	std::string gameDirectory; /* nothing is written or deleted outside it */
	std::string protectedRoot; /* nothing is written or deleted at or below it: the repositories' root */
	uint64_t maxBytes;         /* a larger file, or files of one WriteDir larger together, are not written */
};

/* What SweepLeftovers deleted, and what it could not look at or delete. */
struct SweepReport {
	std::vector<std::string> removed;
	std::vector<std::string> problems;
};

struct RepositoryInfo {
	std::string name; /* <host>/<path> */
	std::string url;
	std::string headBranch;
	std::string headSha;
	int64_t fetchTime; /* Unix seconds of the last fetch, 0 when unknown */
};

/* The ref a watch of the remote's default branch names. */
const char kHeadRef[] = "HEAD";

/* The most entries one listing of a directory (ListTree, WriteDir) takes;
 * beyond it the listing ends with TooLarge. Git trees may share
 * subdirectories, so a small repository can name very many paths. */
const size_t kMaxTreeEntries = 100000;

/* The path with symlinks resolved as far as it exists, without "." and
 * "..", and without a trailing slash. */
Result<std::string> CanonicalPath(const std::string &path);

/* Whether a libgit2 error message tells that a lock file of another
 * process exists: "failed to lock file '...' for writing", or "failed to
 * create locked file '...'" with the reason "File exists". Other reasons
 * (no permission, a full or read-only disk) are no lock to wait for. */
bool IsLockExistsMessage(const std::string &message);

/* Whether a watch can follow ref: HEAD, or a name a branch or tag could
 * have ("main", "release/1.0", "v1.0"), not a full ref name ("refs/...")
 * nor an expression ("main~1", "a..b"). */
bool IsWatchableRef(const std::string &ref);

/* Starts libgit2 with the timeouts of config, limits on what a downloaded
 * pack may make it allocate after the size cap of config, and the CA
 * certificates of caBundle. Returns the reason when that fails, otherwise
 * an empty string. */
std::string Initialize(const Config &config, const std::string &caBundle);
void Shutdown();

/* Whether a repository is at path. */
bool Exists(const std::string &path);

/* Clones remote to path: into a temp directory next to it that is renamed
 * into place, so a repository at path is always complete. When another
 * process put one there in the meantime, that one wins. Redirects are
 * followed only on the remote's host; a remote with more than 10000
 * branches and tags, or a download beyond maxBytes, fails with TooLarge. */
Result<Nothing> Clone(const std::string &path, const Remote &remote, uint64_t maxBytes,
                      const std::atomic<bool> &abort);

/* Fetches every branch and tag, drops those the remote no longer has and
 * follows the remote's default branch. Refused when the repository's remote
 * is no longer an https:// URL the extension accepts. Redirects are
 * followed only on the remote's host; a remote with more than 10000
 * branches and tags, or a repository that would grow beyond maxBytes, fails
 * with TooLarge, the latter with the way out in the error. */
Result<FetchReport> Fetch(const std::string &path, uint64_t maxBytes, const std::atomic<bool> &abort);

/* The commit a revision (branch, tag, SHA, "HEAD", "main~2") names. */
Result<std::string> Resolve(const std::string &path, const std::string &rev);

Result<RefSnapshot> ListRefs(const std::string &path);

/* The commits reachable from rev, newest first, at most maxCount. With a
 * repoPath only those that changed it against all their parents; with
 * since only those authored at or after it, and the walk ends at the first
 * commit committed more than a day before it. */
Result<std::vector<Commit>> Log(const std::string &path, const std::string &rev, const std::string &repoPath,
                                int maxCount, int64_t since);

/* The files that differ between two revisions, below repoPath. */
Result<std::vector<DiffEntry>> Diff(const std::string &path, const std::string &baseRev,
                                    const std::string &headRev, const std::string &repoPath);

/* The entries of the directory repoPath at rev. */
Result<std::vector<TreeEntry>> ListTree(const std::string &path, const std::string &rev,
                                        const std::string &repoPath, bool recursive);

/* The content of the file repoPath at rev; TooLarge beyond maxBytes, before
 * the content is unpacked. */
Result<std::string> ReadFile(const std::string &path, const std::string &rev, const std::string &repoPath,
                             uint64_t maxBytes);

/* Writes the file repoPath at rev to the destination, through a temp file
 * renamed onto it, unless the destination already has that content. The
 * temp file is created anew, never through a symlink. A destination with a
 * part ending in ".git-tmp", the temp files' name, is refused. The changes
 * are in the value also when it failed (WriteDir may have changed files
 * before; WriteFile has none then). */
Result<std::vector<Change>> WriteFile(const std::string &path, const std::string &rev,
                                      const std::string &repoPath, const Destination &destination,
                                      const WritePolicy &policy);

/* Writes every file of the directory repoPath at rev below the destination
 * like WriteFile; symlinks and submodules are left out. With an oldRev, the
 * files oldRev had in that directory and rev no longer has are deleted from
 * the destination first. Refused before anything is touched when the
 * destination or a path of either revision has a part (file or directory)
 * ending in ".git-tmp". */
Result<std::vector<Change>> WriteDir(const std::string &path, const std::string &rev,
                                     const std::string &repoPath, const Destination &destination,
                                     const std::string &oldRev, const WritePolicy &policy);

/* Whether the local file has the content of the file repoPath at rev.
 * Refused where a write-out of it would be (out of the game directory or
 * into the root); a symlink or anything else but a regular file at
 * localFile is Different, never followed or read. */
Result<FileState> CompareFile(const std::string &path, const std::string &rev, const std::string &repoPath,
                              const Destination &localFile, const WritePolicy &policy);

/* Deletes the repository at path from disk: it is first renamed out of the
 * way, so to every reader it is gone at once, even when a crash interrupts
 * the deletion. */
Result<Nothing> Remove(const std::string &path);

/* Every repository below root. Directories that cannot be read are left
 * out; a root that does not exist has none. */
Result<std::vector<RepositoryInfo>> ListRepositories(const std::string &root);

/* Deletes the lock files of the repository at path (those in its top
 * directory and below refs/) that were last changed longer than maxAge
 * ago: left by a process that crashed while holding them. Returns what it
 * deleted. */
std::vector<std::string> RemoveStaleLocks(const std::string &path, std::chrono::seconds maxAge);

/* Deletes what crashed processes left below root: every removal that was
 * interrupted, and every clone temp directory that has not grown for an
 * hour. */
SweepReport SweepLeftovers(const std::string &root);

} // namespace smgit

#endif // SMGIT_REPO_H

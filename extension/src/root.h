/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The names below the root, the convention that lets several servers share
 * it without roles: the repository of a
 * remote is <host>/<path>.git, and next to it are the temp directories of
 * its clones (<host>/<path>.git+tmp-XXXXXX) and removals
 * (<host>/<path>.git+removing-XXXXXX), each with a random name no other
 * process can have taken. "+" is no character of a remote URL's path, so no
 * repository or directory of the root can carry those marks. Knows neither
 * git nor SourceMod. */
#ifndef SMGIT_ROOT_H
#define SMGIT_ROOT_H

#include "remote.h"
#include "status.h"

#include <functional>
#include <string>
#include <system_error>

namespace smgit {

/* What a directory below the root is, by its name. */
enum class RootEntryKind {
	Repository, /* <name>.git */
	CloneTemp,  /* a clone in progress, or one a crash abandoned */
	Removal,    /* a removal in progress, or one a crash interrupted */
	Directory,  /* a directory of hosts and paths, which may hold any of these */
};

struct RootEntry {
	RootEntryKind kind;
	std::string path;
};

/* The directory of a remote's repository below root. */
std::string RepositoryPath(const std::string &root, const Remote &remote);

/* The name of the repository at path below root ("<host>/<path>"), as
 * RepositoryPath made it. */
std::string RepositoryNameAt(const std::string &root, const std::string &path);

/* What a directory named name below the root is. */
RootEntryKind ClassifyRootEntry(const std::string &name);

/* Creates a new, empty clone temp directory next to the repository path
 * and returns it. */
Result<std::string> CreateCloneTemp(const std::string &repositoryPath);

/* Creates a new, empty removal directory next to the repository path and
 * returns it; the repository is renamed to RemovedRepositoryIn of it. */
Result<std::string> CreateRemovalDirectory(const std::string &repositoryPath);
std::string RemovedRepositoryIn(const std::string &removalDirectory);

/* Calls visit with every repository, clone temp and removal below root,
 * and does not look into them, nor into directories deeper than the root
 * can have. visit may delete what it is given. Directories that cannot be
 * read are skipped; returns the error that ended the walk early, such as
 * a root that does not exist or is no directory. */
std::error_code WalkRoot(const std::string &root, const std::function<void(const RootEntry &)> &visit);

} // namespace smgit

#endif // SMGIT_ROOT_H

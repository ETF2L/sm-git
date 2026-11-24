/* Copyright 2026 ETF2L Contributors, Dave */
#include "root.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <filesystem>
#include <random>

namespace fs = std::filesystem;

namespace smgit {

namespace {

const char kRepositorySuffix[] = ".git";
const char kCloneTempMark[] = ".git+tmp-";
const char kRemovalMark[] = ".git+removing-";
const char kRemovedRepository[] = "repository";
const char kUniqueChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const int kUniqueLength = 6;
const int kUniqueAttempts = 100;
const int kMaxRootDepth = 32;
const size_t kRepositorySuffixLength = sizeof(kRepositorySuffix) - 1;

bool EndsWith(const std::string &text, const char *suffix)
{
	size_t length = strlen(suffix);
	return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

bool HasMark(const std::string &name, const char *mark)
{
	return name.find(mark) != std::string::npos;
}

/* A repository's path without its ".git", the start of the names of its
 * temp directories next to it. */
std::string StemOf(const std::string &repositoryPath)
{
	return repositoryPath.substr(0, repositoryPath.size() - kRepositorySuffixLength);
}

/* Creates a new directory named prefix and six random characters, which no
 * other process or thread can have created as well: mkdir fails on a name
 * that exists, wherever the process runs. The mode follows the umask, as
 * for every other directory of the repository. */
Result<std::string> CreateUniqueDirectory(const std::string &prefix)
{
	std::random_device random;
	std::uniform_int_distribution<size_t> pick(0, sizeof(kUniqueChars) - 2);
	for (int attempt = 0; attempt < kUniqueAttempts; attempt++) {
		std::string candidate = prefix;
		for (int i = 0; i < kUniqueLength; i++)
			candidate += kUniqueChars[pick(random)];
		if (mkdir(candidate.c_str(), 0777) == 0)
			return Ok(candidate);
		if (errno != EEXIST)
			return Fail<std::string>(Status::Failed, "cannot create " + candidate + ": " + strerror(errno));
	}
	return Fail<std::string>(Status::Failed, "cannot find a free name for " + prefix + "XXXXXX");
}

} // namespace

std::string RepositoryPath(const std::string &root, const Remote &remote)
{
	return root + "/" + remote.name + kRepositorySuffix;
}

std::string RepositoryNameAt(const std::string &root, const std::string &path)
{
	return path.substr(root.size() + 1, path.size() - root.size() - 1 - kRepositorySuffixLength);
}

RootEntryKind ClassifyRootEntry(const std::string &name)
{
	if (HasMark(name, kRemovalMark))
		return RootEntryKind::Removal;
	if (HasMark(name, kCloneTempMark))
		return RootEntryKind::CloneTemp;
	if (EndsWith(name, kRepositorySuffix))
		return RootEntryKind::Repository;
	return RootEntryKind::Directory;
}

Result<std::string> CreateCloneTemp(const std::string &repositoryPath)
{
	return CreateUniqueDirectory(StemOf(repositoryPath) + kCloneTempMark);
}

Result<std::string> CreateRemovalDirectory(const std::string &repositoryPath)
{
	return CreateUniqueDirectory(StemOf(repositoryPath) + kRemovalMark);
}

std::string RemovedRepositoryIn(const std::string &removalDirectory)
{
	return removalDirectory + "/" + kRemovedRepository;
}

std::error_code WalkRoot(const std::string &root, const std::function<void(const RootEntry &)> &visit)
{
	std::error_code failure;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, failure), end;
	for (; !failure && it != end; it.increment(failure)) {
		std::error_code typeFailure;
		if (!it->is_directory(typeFailure))
			continue;
		RootEntryKind kind = ClassifyRootEntry(it->path().filename().string());
		if (kind == RootEntryKind::Directory) {
			if (it.depth() >= kMaxRootDepth)
				it.disable_recursion_pending();
			continue;
		}
		/* Before the visit, which may delete it. */
		it.disable_recursion_pending();
		visit(RootEntry{kind, it->path().string()});
	}
	return failure;
}

} // namespace smgit

/* Copyright 2026 ETF2L Contributors, Dave */
#include "repo.h"

#include "root.h"
#include "serverlog.h"

#include <git2.h>
#include <mbedtls/sha1.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace smgit {

namespace {

const char kRemoteName[] = "origin";
const char kBranchRefspec[] = "+refs/heads/*:refs/heads/*";
const char kTagRefspec[] = "+refs/tags/*:refs/tags/*";
const char kBranchPrefix[] = "refs/heads/";
const char kTagPrefix[] = "refs/tags/";
const char kWriteTempSuffix[] = ".git-tmp";
const char kLockSuffix[] = ".lock";
const char kPeeledSuffix[] = "^{}";
const char kGrowthAdvice[] = "; a repository only grows, so remove it to have it cloned anew, smaller, or raise "
                             "max_size_mb";
const int kLockRetries = 25;
const std::chrono::milliseconds kLockRetryDelay(200);
const std::chrono::minutes kStaleLockAge(10);
const std::chrono::hours kStaleTempAge(1);
/* How much earlier than a Log's since a commit may have been committed and
 * still be authored after it: clocks of committers are skewed, and a rebase
 * commits old work anew. The walk ends at the first commit committed before
 * since less this. */
const int64_t kLogSkewTolerance = 24 * 60 * 60;
const int kMaxTreeDepth = 64;
/* The most bytes the paths of one listing take together: entries may have
 * long names and nest deeply, so kMaxTreeEntries alone does not bound it. */
const size_t kMaxTreePathBytes = 32 * 1024 * 1024;
/* The most branches and tags a remote may have. Each one becomes a file of
 * its own, which the size cap, counting bytes, would barely notice. */
const size_t kMaxRemoteRefs = 10000;
/* The fewest bytes one object takes in a pack: a header byte and the
 * shortest zlib stream. A pack within the size cap cannot hold more objects
 * than the cap has bytes of this. */
const size_t kMinPackedObjectBytes = 8;
const size_t kHashChunkBytes = 64 * 1024;

template <typename T, void (*Free)(T *)>
struct GitDeleter {
	void operator()(T *object) const { Free(object); }
};
template <typename T, void (*Free)(T *)>
using GitPtr = std::unique_ptr<T, GitDeleter<T, Free>>;

using RepositoryPtr = GitPtr<git_repository, git_repository_free>;
using RemotePtr = GitPtr<git_remote, git_remote_free>;
using ObjectPtr = GitPtr<git_object, git_object_free>;
using CommitPtr = GitPtr<git_commit, git_commit_free>;
using TreePtr = GitPtr<git_tree, git_tree_free>;
using TreeEntryPtr = GitPtr<git_tree_entry, git_tree_entry_free>;
using BlobPtr = GitPtr<git_blob, git_blob_free>;
using ReferencePtr = GitPtr<git_reference, git_reference_free>;
using ReferenceIteratorPtr = GitPtr<git_reference_iterator, git_reference_iterator_free>;
using RevwalkPtr = GitPtr<git_revwalk, git_revwalk_free>;
using DiffPtr = GitPtr<git_diff, git_diff_free>;
using OdbPtr = GitPtr<git_odb, git_odb_free>;

std::string LastGitMessage()
{
	const git_error *last = git_error_last();
	return last && last->message ? last->message : "unknown libgit2 error";
}

bool IsNetworkErrorClass(int errorClass)
{
	return errorClass == GIT_ERROR_NET || errorClass == GIT_ERROR_SSL || errorClass == GIT_ERROR_HTTP;
}

/* The status of a failed libgit2 call that talked to the remote. */
Status StatusOfRemoteError(int code)
{
	if (code == GIT_TIMEOUT)
		return Status::Timeout;
	if (code == GIT_ECERTIFICATE)
		return Status::Network;
	const git_error *last = git_error_last();
	if (last && IsNetworkErrorClass(last->klass))
		return Status::Network;
	return Status::Failed;
}

template <typename T>
Result<T> GitFail(Status status, const std::string &what)
{
	return Fail<T>(status, what + ": " + LastGitMessage());
}

std::string ShaOf(const git_oid *id)
{
	if (git_oid_is_zero(id))
		return {};
	char text[GIT_OID_SHA1_HEXSIZE + 1];
	git_oid_tostr(text, sizeof(text), id);
	return text;
}

std::string JoinPath(const std::string &directory, const std::string &name)
{
	return directory.empty() ? name : directory + "/" + name;
}

bool StartsWith(const std::string &text, const char *prefix)
{
	return text.compare(0, strlen(prefix), prefix) == 0;
}

bool EndsWith(const std::string &text, const char *suffix)
{
	size_t length = strlen(suffix);
	return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

/* Whether a part of a write-out's path has the name of its temp files: in
 * a repository with "a" and "a.git-tmp" (a file or a directory), writing
 * "a" would replace or trip over "a.git-tmp". */
bool HasWriteTempName(const std::string &path)
{
	for (size_t start = 0; start <= path.size();) {
		size_t end = path.find('/', start);
		size_t length = (end == std::string::npos ? path.size() : end) - start;
		if (EndsWith(path.substr(start, length), kWriteTempSuffix))
			return true;
		if (end == std::string::npos)
			return false;
		start = end + 1;
	}
	return false;
}

Result<Nothing> RefuseWriteTempName(const std::string &path)
{
	return Fail<Nothing>(Status::Refused, "\"" + path + "\" has a part ending in \"" + kWriteTempSuffix +
	                                      "\", the name of the write-out's temp files");
}

RepositoryPtr OpenRepository(const std::string &path)
{
	git_repository *repository = nullptr;
	if (git_repository_open_bare(&repository, path.c_str()) < 0)
		return nullptr;
	return RepositoryPtr(repository);
}

template <typename T>
Result<T> NotCloned(const std::string &path)
{
	return Fail<T>(Status::NotCloned, "no repository at " + path);
}

/* The commit rev names in the repository. */
Result<CommitPtr> ResolveCommit(git_repository *repository, const std::string &rev)
{
	git_object *named = nullptr;
	if (git_revparse_single(&named, repository, rev.c_str()) < 0)
		return GitFail<CommitPtr>(Status::NotFound, "no revision \"" + rev + "\"");
	ObjectPtr namedOwner(named);

	git_object *commit = nullptr;
	if (git_object_peel(&commit, named, GIT_OBJECT_COMMIT) < 0)
		return GitFail<CommitPtr>(Status::NotFound, "\"" + rev + "\" names no commit");
	return Ok(CommitPtr(reinterpret_cast<git_commit *>(commit)));
}

/* The entry at repoPath in the commit's tree; null when there is none. */
TreeEntryPtr FindEntry(git_commit *commit, const std::string &repoPath)
{
	git_tree *root = nullptr;
	if (git_commit_tree(&root, commit) < 0)
		return nullptr;
	TreePtr rootOwner(root);

	git_tree_entry *entry = nullptr;
	if (git_tree_entry_bypath(&entry, root, repoPath.c_str()) < 0)
		return nullptr;
	return TreeEntryPtr(entry);
}

/* The tree of the directory repoPath in the commit; the top one for "". */
Result<TreePtr> FindDirectory(git_repository *repository, git_commit *commit, const std::string &repoPath)
{
	git_tree *tree = nullptr;
	if (repoPath.empty()) {
		if (git_commit_tree(&tree, commit) < 0)
			return GitFail<TreePtr>(Status::Failed, "cannot read the commit's tree");
		return Ok(TreePtr(tree));
	}

	TreeEntryPtr entry = FindEntry(commit, repoPath);
	if (!entry || git_tree_entry_type(entry.get()) != GIT_OBJECT_TREE)
		return Fail<TreePtr>(Status::NotFound, "no directory \"" + repoPath + "\" in the commit");
	if (git_tree_lookup(&tree, repository, git_tree_entry_id(entry.get())) < 0)
		return GitFail<TreePtr>(Status::Failed, "cannot read the directory \"" + repoPath + "\"");
	return Ok(TreePtr(tree));
}

/* The bytes of a blob, from its header: nothing is unpacked. */
Result<uint64_t> BlobSize(git_repository *repository, const git_oid *id)
{
	git_odb *objects = nullptr;
	if (git_repository_odb(&objects, repository) < 0)
		return GitFail<uint64_t>(Status::Failed, "cannot open the object database");
	OdbPtr objectsOwner(objects);

	size_t size = 0;
	git_object_t type;
	if (git_odb_read_header(&size, &type, objects, id) < 0)
		return GitFail<uint64_t>(Status::Failed, "cannot read the size of " + ShaOf(id));
	return Ok(static_cast<uint64_t>(size));
}

/* The entry of the file repoPath in the commit; symlinks are no files. */
Result<TreeEntryPtr> FindFile(git_commit *commit, const std::string &repoPath)
{
	TreeEntryPtr entry = FindEntry(commit, repoPath);
	if (!entry || git_tree_entry_filemode(entry.get()) == GIT_FILEMODE_LINK ||
	    git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB)
		return Fail<TreeEntryPtr>(Status::NotFound, "no file \"" + repoPath + "\" in the commit");
	return Ok(std::move(entry));
}

EntryType TypeOfMode(git_filemode_t mode)
{
	switch (mode) {
	case GIT_FILEMODE_TREE:   return EntryType::Directory;
	case GIT_FILEMODE_LINK:   return EntryType::Symlink;
	case GIT_FILEMODE_COMMIT: return EntryType::Submodule;
	default:                  return EntryType::File;
	}
}

/* The entries of a listing so far, and the bytes their paths take. */
struct Listing {
	std::vector<TreeEntry> entries;
	size_t pathBytes = 0;
};

/* Appends the entries of tree, named below prefix, and with recursive those
 * of its directories. An entry whose name could leave its directory makes
 * the whole listing fail, and so does a listing beyond kMaxTreeEntries
 * entries or kMaxTreePathBytes bytes of paths. */
Result<Nothing> CollectEntries(git_repository *repository, git_odb *objects, git_tree *tree,
                               const std::string &prefix, bool recursive, int depth, Listing &listing)
{
	if (depth > kMaxTreeDepth)
		return Fail<Nothing>(Status::Refused, "the repository nests directories deeper than " +
		                                      std::to_string(kMaxTreeDepth) + " levels");
	size_t count = git_tree_entrycount(tree);
	for (size_t i = 0; i < count; i++) {
		if (listing.entries.size() >= kMaxTreeEntries)
			return Fail<Nothing>(Status::TooLarge, "the directory has more than " + std::to_string(kMaxTreeEntries) +
			                                       " entries");
		const git_tree_entry *entry = git_tree_entry_byindex(tree, i);
		std::string name = git_tree_entry_name(entry);
		if (!IsSafeEntryName(name))
			return Fail<Nothing>(Status::Refused, "the repository has an entry with the unsafe name \"" + name + "\"");
		listing.pathBytes += prefix.size() + 1 + name.size();
		if (listing.pathBytes > kMaxTreePathBytes)
			return Fail<Nothing>(Status::TooLarge, "the paths of the directory's entries take more than " +
			                                       std::to_string(kMaxTreePathBytes / (1024 * 1024)) + " MB");

		const git_oid *id = git_tree_entry_id(entry);
		git_filemode_t mode = git_tree_entry_filemode(entry);
		EntryType type = TypeOfMode(mode);
		size_t size = 0;
		if (type == EntryType::File || type == EntryType::Symlink) {
			git_object_t objectType;
			if (git_odb_read_header(&size, &objectType, objects, id) < 0)
				return GitFail<Nothing>(Status::Failed, "cannot read \"" + JoinPath(prefix, name) + "\"");
		}
		std::string path = JoinPath(prefix, name);
		listing.entries.push_back(TreeEntry{path, type, size, ShaOf(id), mode == GIT_FILEMODE_BLOB_EXECUTABLE});

		if (type != EntryType::Directory || !recursive)
			continue;
		git_tree *child = nullptr;
		if (git_tree_lookup(&child, repository, id) < 0)
			return GitFail<Nothing>(Status::Failed, "cannot read the directory \"" + path + "\"");
		TreePtr childOwner(child);
		Result<Nothing> collected = CollectEntries(repository, objects, child, path, true, depth + 1, listing);
		if (!collected.ok())
			return collected;
	}
	return {};
}

Result<std::vector<TreeEntry>> ListDirectory(git_repository *repository, git_commit *commit,
                                             const std::string &repoPath, bool recursive)
{
	using Entries = std::vector<TreeEntry>;
	Result<TreePtr> directory = FindDirectory(repository, commit, repoPath);
	if (!directory.ok())
		return FailLike<Entries>(directory);

	git_odb *objects = nullptr;
	if (git_repository_odb(&objects, repository) < 0)
		return GitFail<Entries>(Status::Failed, "cannot open the object database");
	OdbPtr objectsOwner(objects);

	Listing listing;
	Result<Nothing> collected = CollectEntries(repository, objects, directory.value.get(), repoPath, recursive, 0,
	                                           listing);
	if (!collected.ok())
		return FailLike<Entries>(collected);
	return Ok(std::move(listing.entries));
}

/* The files of the directory repoPath in the commit, by their path below
 * it. */
Result<std::map<std::string, TreeEntry>> FilesBelow(git_repository *repository, git_commit *commit,
                                                   const std::string &repoPath)
{
	using Files = std::map<std::string, TreeEntry>;
	Result<std::vector<TreeEntry>> entries = ListDirectory(repository, commit, repoPath, true);
	if (!entries.ok())
		return FailLike<Files>(entries);

	size_t prefixLength = repoPath.empty() ? 0 : repoPath.size() + 1;
	Files files;
	for (TreeEntry &entry : entries.value) {
		if (entry.type == EntryType::File)
			files.emplace(entry.path.substr(prefixLength), std::move(entry));
	}
	return Ok(std::move(files));
}

bool FileExists(const std::string &file)
{
	struct stat info;
	return lstat(file.c_str(), &info) == 0;
}

/* A file descriptor, closed when it goes out of scope. */
class FileDescriptor {
public:
	explicit FileDescriptor(int fd) : m_fd(fd) {}
	~FileDescriptor()
	{
		if (m_fd >= 0)
			close(m_fd);
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;

	bool isOpen() const { return m_fd >= 0; }
	int get() const { return m_fd; }

private:
	int m_fd;
};

/* The git blob id of the size bytes the descriptor reads, the SHA-1 of
 * "blob <size>", a NUL and the content, read in pieces; none when it reads
 * another number of bytes. */
std::optional<git_oid> HashBlobFrom(int fd, uint64_t size)
{
	mbedtls_sha1_context sha;
	mbedtls_sha1_init(&sha);
	std::unique_ptr<mbedtls_sha1_context, void (*)(mbedtls_sha1_context *)> shaOwner(&sha, mbedtls_sha1_free);
	std::string header = "blob " + std::to_string(size);
	if (mbedtls_sha1_starts(&sha) != 0 ||
	    mbedtls_sha1_update(&sha, reinterpret_cast<const unsigned char *>(header.c_str()), header.size() + 1) != 0)
		return std::nullopt;

	std::vector<unsigned char> chunk(kHashChunkBytes);
	uint64_t remaining = size;
	for (;;) {
		ssize_t got = read(fd, chunk.data(), chunk.size());
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0 || static_cast<uint64_t>(got) > remaining)
			return std::nullopt;
		if (got == 0)
			break;
		remaining -= static_cast<uint64_t>(got);
		if (mbedtls_sha1_update(&sha, chunk.data(), static_cast<size_t>(got)) != 0)
			return std::nullopt;
	}
	unsigned char digest[GIT_OID_SHA1_SIZE];
	if (remaining != 0 || mbedtls_sha1_finish(&sha, digest) != 0)
		return std::nullopt;
	git_oid id;
	git_oid_fromraw(&id, digest);
	return id;
}

/* Whether the local file is a regular file with exactly the blob's
 * content. A symlink at file is not followed and a FIFO not waited on:
 * anything but a regular file counts as other content. */
bool HasContentOf(const std::string &file, const git_oid *blobId, uint64_t blobSize)
{
	FileDescriptor in(open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	if (!in.isOpen())
		return false;
	struct stat info;
	if (fstat(in.get(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<uint64_t>(info.st_size) != blobSize)
		return false;
	std::optional<git_oid> fileId = HashBlobFrom(in.get(), blobSize);
	return fileId && git_oid_equal(&*fileId, blobId) != 0;
}

std::string ErrnoText()
{
	return strerror(errno);
}

/* Creates the temp file of a write-out anew. Whatever has its name, a temp
 * file a crash left or a symlink planted there, is unlinked, which never
 * follows a symlink, and the file is only created when nothing took the
 * name in between. */
int CreateWriteTemp(const std::string &temp)
{
	unlink(temp.c_str());
	return open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}

/* Writes all size bytes; 0, or the errno of the write that failed. */
int WriteAll(int fd, const char *data, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, data, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0)
			return errno;
		if (written == 0)
			return EIO;
		data += written;
		size -= static_cast<size_t>(written);
	}
	return 0;
}

/* Writes the blob to the file through a temp file next to it. */
Result<Nothing> WriteBlobTo(git_repository *repository, const git_oid *blobId, bool isExecutable,
                            const std::string &file)
{
	git_blob *blob = nullptr;
	if (git_blob_lookup(&blob, repository, blobId) < 0)
		return GitFail<Nothing>(Status::Failed, "cannot read the content for " + file);
	BlobPtr blobOwner(blob);

	std::error_code failure;
	fs::create_directories(fs::path(file).parent_path(), failure);
	if (failure)
		return Fail<Nothing>(Status::Failed, "cannot create the directory of " + file + ": " + failure.message());

	std::string temp = file + kWriteTempSuffix;
	int out = CreateWriteTemp(temp);
	if (out < 0)
		return Fail<Nothing>(Status::Failed, "cannot create " + temp + ": " + ErrnoText());
	size_t size = static_cast<size_t>(git_blob_rawsize(blob));
	/* The reason is taken from errno right after the call that failed,
	 * before any other call can change it. */
	int writeError = WriteAll(out, static_cast<const char *>(git_blob_rawcontent(blob)), size);
	if (writeError == 0 && fchmod(out, isExecutable ? 0755 : 0644) != 0)
		writeError = errno;
	if (close(out) != 0 && writeError == 0)
		writeError = errno;
	if (writeError == 0) {
		/* A directory whose files the repository dropped may be left where
		 * the file goes; rmdir only takes it when it is empty. */
		rmdir(file.c_str());
		if (rename(temp.c_str(), file.c_str()) != 0)
			writeError = errno;
	}
	if (writeError == 0)
		return {};
	unlink(temp.c_str());
	return Fail<Nothing>(Status::Failed, "cannot write " + file + ": " + strerror(writeError));
}

/* Why a write-out may not touch the destination; OK when it may. The
 * directory it is in is resolved as far as it exists, so a symlink on the
 * way counts where it leads. The file itself is never followed: it is
 * replaced or deleted, not written through. */
Result<Nothing> CheckDestination(const Destination &destination, const WritePolicy &policy)
{
	fs::path file(destination.absolute);
	Result<std::string> directory = CanonicalPath(file.parent_path().string());
	if (!directory.ok())
		return FailLike<Nothing>(directory);
	std::string place = directory.value + "/" + file.filename().string();
	if (!IsAtOrBelow(place, policy.gameDirectory))
		return Fail<Nothing>(Status::Refused, destination.display + " leads out of the game directory");
	if (IsAtOrBelow(place, policy.protectedRoot))
		return Fail<Nothing>(Status::Refused, destination.display + " is inside the repositories' root");
	return {};
}

/* Brings one destination file to the content of a repository file and
 * notes it in changes when that took a write. */
Result<Nothing> SyncFile(git_repository *repository, const TreeEntry &source, const Destination &destination,
                         const WritePolicy &policy, std::vector<Change> &changes)
{
	Result<Nothing> allowed = CheckDestination(destination, policy);
	if (!allowed.ok())
		return allowed;
	if (source.size > policy.maxBytes)
		return Fail<Nothing>(Status::TooLarge, "\"" + source.path + "\" has " + std::to_string(source.size) +
		                                       " bytes, more than a write-out takes");

	git_oid blobId;
	git_oid_fromstr(&blobId, source.sha.c_str());
	if (HasContentOf(destination.absolute, &blobId, source.size))
		return {};

	Result<Nothing> written = WriteBlobTo(repository, &blobId, source.isExecutable, destination.absolute);
	if (!written.ok())
		return written;
	changes.push_back(Change{destination.display, ChangeKind::Written});
	return {};
}

/* A failed write-out with what it changed before it failed. */
Result<std::vector<Change>> FailWithChanges(const Result<Nothing> &failed, std::vector<Change> changes)
{
	Result<std::vector<Change>> result = FailLike<std::vector<Change>>(failed);
	result.value = std::move(changes);
	return result;
}

Destination Below(const Destination &directory, const std::string &relativePath)
{
	return Destination{JoinPath(directory.absolute, relativePath), JoinPath(directory.display, relativePath)};
}

RefSnapshot ReadRefs(git_repository *repository)
{
	RefSnapshot snapshot;

	git_reference *head = nullptr;
	if (git_repository_head(&head, repository) == 0) {
		ReferencePtr headOwner(head);
		snapshot.headBranch = git_reference_shorthand(head);
		snapshot.headSha = ShaOf(git_reference_target(head));
	}

	git_reference_iterator *iterator = nullptr;
	if (git_reference_iterator_new(&iterator, repository) < 0)
		return snapshot;
	ReferenceIteratorPtr iteratorOwner(iterator);

	git_reference *next = nullptr;
	while (git_reference_next(&next, iterator) == 0) {
		ReferencePtr reference(next);
		std::string fullName = git_reference_name(next);
		bool isTag = StartsWith(fullName, kTagPrefix);
		if (!isTag && !StartsWith(fullName, kBranchPrefix))
			continue;

		git_object *commit = nullptr;
		if (git_reference_peel(&commit, next, GIT_OBJECT_COMMIT) < 0)
			continue;
		ObjectPtr commitOwner(commit);
		snapshot.refs.push_back(Ref{git_reference_shorthand(next), ShaOf(git_object_id(commit)), isTag});
	}
	return snapshot;
}

bool SameRefs(const RefSnapshot &a, const RefSnapshot &b)
{
	if (a.headBranch != b.headBranch || a.headSha != b.headSha || a.refs.size() != b.refs.size())
		return false;
	std::map<std::string, std::string> shas;
	for (const Ref &ref : a.refs)
		shas[(ref.isTag ? kTagPrefix : kBranchPrefix) + ref.name] = ref.sha;
	for (const Ref &ref : b.refs) {
		auto found = shas.find((ref.isTag ? kTagPrefix : kBranchPrefix) + ref.name);
		if (found == shas.end() || found->second != ref.sha)
			return false;
	}
	return true;
}

/* The bytes of every file below a directory. */
uint64_t DirectorySize(const std::string &directory)
{
	uint64_t total = 0;
	std::error_code failure;
	for (fs::recursive_directory_iterator it(directory, failure), end; !failure && it != end; it.increment(failure)) {
		std::error_code sizeFailure;
		if (it->is_regular_file(sizeFailure)) {
			uint64_t size = it->file_size(sizeFailure);
			if (!sizeFailure)
				total += size;
		}
	}
	return total;
}

/* What the callbacks of a clone or fetch watch over. */
struct Transfer {
	const std::atomic<bool> &abort;
	uint64_t bytesBefore;
	uint64_t maxBytes;
	bool isTooLarge = false;
};

bool IsAborted(void *payload)
{
	return static_cast<Transfer *>(payload)->abort;
}

/* Before every connection to the remote. */
int OnRemoteReady(git_remote *, int, void *payload)
{
	return IsAborted(payload) ? -1 : 0;
}

/* After the TLS handshake of every connection; the certificate itself is
 * judged by libgit2 as without this callback. */
int OnCertificateCheck(git_cert *, int, const char *, void *payload)
{
	return IsAborted(payload) ? -1 : GIT_PASSTHROUGH;
}

/* For every progress message the remote sends while it packs. */
int OnSidebandProgress(const char *, int, void *payload)
{
	return IsAborted(payload) ? -1 : 0;
}

/* For every piece of the pack that arrives. */
int OnTransferProgress(const git_indexer_progress *progress, void *payload)
{
	Transfer *transfer = static_cast<Transfer *>(payload);
	if (transfer->abort)
		return -1;
	if (transfer->bytesBefore + progress->received_bytes > transfer->maxBytes) {
		transfer->isTooLarge = true;
		return -1;
	}
	return 0;
}

git_remote_callbacks CallbacksFor(Transfer &transfer)
{
	git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
	callbacks.remote_ready = OnRemoteReady;
	callbacks.certificate_check = OnCertificateCheck;
	callbacks.sideband_progress = OnSidebandProgress;
	callbacks.transfer_progress = OnTransferProgress;
	callbacks.payload = &transfer;
	return callbacks;
}

Result<Nothing> Aborted()
{
	return Fail<Nothing>(Status::Failed, "aborted: the extension is unloading");
}

/* Whether a failed libgit2 call ran into a lock file of another process.
 * Not every caller passes GIT_ELOCKED on, so the message counts too (see
 * IsLockExistsMessage). */
bool IsLockContention(int code)
{
	if (code == GIT_ELOCKED)
		return true;
	const git_error *last = git_error_last();
	return code < 0 && last && last->message && IsLockExistsMessage(last->message);
}

/* Runs a libgit2 call that writes refs of the repository at path, again
 * after a short wait for as long as another process holds a lock it needs.
 * When the lock is still held after the retries and its file is old, the
 * process that took it is taken to have crashed: stale lock files are
 * deleted and the call runs once more. An abort ends the waiting with the
 * call's last failure. */
template <typename Call>
int RetryWhileLocked(const std::string &path, const std::atomic<bool> &abort, Call call)
{
	int code = 0;
	for (int attempt = 0; attempt < kLockRetries; attempt++) {
		code = call();
		if (!IsLockContention(code) || abort)
			return code;
		std::this_thread::sleep_for(kLockRetryDelay);
	}
	if (abort || RemoveStaleLocks(path, kStaleLockAge).empty())
		return code;
	return call();
}

/* Whether HEAD already points at the branch. */
bool HeadIs(git_repository *repository, const std::string &branchRef)
{
	git_reference *head = nullptr;
	if (git_reference_lookup(&head, repository, kHeadRef) < 0)
		return false;
	ReferencePtr headOwner(head);
	const char *target = git_reference_symbolic_target(head);
	return target && branchRef == target;
}

/* How many branches and tags the connected remote advertises; peeled tags
 * ("v1^{}") are no refs of their own. */
Result<size_t> CountBranchesAndTags(git_remote *remote)
{
	const git_remote_head **heads = nullptr;
	size_t headCount = 0;
	if (git_remote_ls(&heads, &headCount, remote) < 0)
		return GitFail<size_t>(Status::Failed, "cannot list the remote's branches and tags");
	size_t count = 0;
	for (size_t i = 0; i < headCount; i++) {
		std::string name = heads[i]->name;
		bool isBranchOrTag = StartsWith(name, kBranchPrefix) || StartsWith(name, kTagPrefix);
		if (isBranchOrTag && !EndsWith(name, kPeeledSuffix))
			count++;
	}
	return Ok(count);
}

std::string MegabytesOf(uint64_t bytes)
{
	char text[32];
	snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
	return text;
}

/* Downloads what the remote has and the repository lacks, moves and prunes
 * its branches and tags and points HEAD at the remote's default branch.
 * sizeCapAdvice follows the error when the download would pass the size
 * cap. */
Result<Nothing> SyncWithRemote(git_repository *repository, const std::string &path, uint64_t maxBytes,
                               const std::string &sizeCapAdvice, const std::atomic<bool> &abort)
{
	git_remote *remote = nullptr;
	if (git_remote_lookup(&remote, repository, kRemoteName) < 0)
		return GitFail<Nothing>(Status::Failed, "the repository has no remote");
	RemotePtr remoteOwner(remote);

	/* The repository's own config names the remote; it must still be one the
	 * extension would have cloned. */
	std::string refusal;
	const char *url = git_remote_url(remote);
	if (!url || !ParseRemoteUrl(url, refusal))
		return Fail<Nothing>(Status::Refused, "the repository's remote is not accepted: " + refusal);

	/* The callbacks end the work early when the extension unloads. While
	 * libgit2 waits for the remote to answer, none of them runs: only
	 * connect_timeout and stall_timeout bound that. */
	Transfer transfer{abort, DirectorySize(path), maxBytes};
	git_remote_callbacks callbacks = CallbacksFor(transfer);

	/* Redirects to another host are refused, so the remote stays on the
	 * host of its accepted URL; libgit2 refuses one from https:// to
	 * another scheme in any case. */
	git_remote_connect_options connectOptions = GIT_REMOTE_CONNECT_OPTIONS_INIT;
	connectOptions.callbacks = callbacks;
	connectOptions.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
	int code = git_remote_connect_ext(remote, GIT_DIRECTION_FETCH, &connectOptions);
	if (code < 0 && abort)
		return Aborted();
	if (code < 0)
		return GitFail<Nothing>(StatusOfRemoteError(code), "cannot connect to the remote");

	Result<size_t> refCount = CountBranchesAndTags(remote);
	if (!refCount.ok() || refCount.value > kMaxRemoteRefs) {
		git_remote_disconnect(remote);
		return refCount.ok() ? Fail<Nothing>(Status::TooLarge, "the remote has " + std::to_string(refCount.value) +
		                                                       " branches and tags, more than the " +
		                                                       std::to_string(kMaxRemoteRefs) + " a repository may have")
		                     : FailLike<Nothing>(refCount);
	}

	git_buf defaultBranch = GIT_BUF_INIT;
	bool hasDefaultBranch = git_remote_default_branch(&defaultBranch, remote) == 0;
	std::string defaultBranchName = hasDefaultBranch ? std::string(defaultBranch.ptr, defaultBranch.size) : "";
	git_buf_dispose(&defaultBranch);

	git_fetch_options fetchOptions = GIT_FETCH_OPTIONS_INIT;
	fetchOptions.callbacks = callbacks;
	fetchOptions.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
	fetchOptions.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
	/* At its end the download rewrites the repository's "shallow" file under
	 * a lock, which another process fetching the same repository may hold.
	 * The pack is stored by then, so a retry reconnects, finds nothing left
	 * to download and writes nothing: libgit2 writes the file only after a
	 * pack. */
	code = RetryWhileLocked(path, abort, [&] {
		int downloaded = git_remote_download(remote, nullptr, &fetchOptions);
		if (downloaded < 0 && IsLockContention(downloaded))
			git_remote_disconnect(remote);
		return downloaded;
	});
	if (code < 0) {
		git_remote_disconnect(remote);
		if (transfer.isTooLarge)
			return Fail<Nothing>(Status::TooLarge, "the repository would grow beyond the size cap of " +
			                                       MegabytesOf(maxBytes) + " (it has " +
			                                       MegabytesOf(transfer.bytesBefore) + " on disk)" + sizeCapAdvice);
		if (abort)
			return Aborted();
		return GitFail<Nothing>(StatusOfRemoteError(code), "cannot download from the remote");
	}

	code = RetryWhileLocked(path, abort, [&] {
		return git_remote_update_tips(remote, &callbacks, GIT_REMOTE_UPDATE_FETCHHEAD,
		                              GIT_REMOTE_DOWNLOAD_TAGS_NONE, nullptr);
	});
	if (code == 0)
		code = RetryWhileLocked(path, abort, [&] { return git_remote_prune(remote, &callbacks); });
	git_remote_disconnect(remote);
	if (code < 0 && abort)
		return Aborted();
	if (code < 0)
		return GitFail<Nothing>(Status::Failed, "cannot update the branches and tags");

	if (!hasDefaultBranch || HeadIs(repository, defaultBranchName))
		return {};
	code = RetryWhileLocked(path, abort,
	                        [&] { return git_repository_set_head(repository, defaultBranchName.c_str()); });
	if (code < 0 && abort)
		return Aborted();
	if (code < 0)
		return GitFail<Nothing>(Status::Failed, "cannot follow the default branch " + defaultBranchName);
	return {};
}

void RemoveDirectory(const std::string &directory)
{
	std::error_code ignored;
	fs::remove_all(directory, ignored);
}

/* When a file or directory was last changed; 0 when that is unknown. */
time_t ModifiedTime(const std::string &path)
{
	struct stat info;
	return lstat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

/* When a clone temp directory last grew: the newest change of the
 * directory, of its pack directory and of the files in it, where libgit2
 * writes what it downloads. */
time_t LastActivityOfClone(const std::string &directory)
{
	std::string packs = directory + "/objects/pack";
	time_t newest = std::max(ModifiedTime(directory), ModifiedTime(packs));
	std::error_code failure;
	for (fs::directory_iterator it(packs, failure), end; !failure && it != end; it.increment(failure))
		newest = std::max(newest, ModifiedTime(it->path().string()));
	return newest;
}

bool IsOlderThan(time_t modified, std::chrono::seconds age)
{
	return modified != 0 && time(nullptr) - modified > static_cast<time_t>(age.count());
}

/* Deletes a leftover and notes it in the report. */
void RemoveLeftover(const std::string &path, SweepReport &report)
{
	std::error_code failure;
	fs::remove_all(path, failure);
	if (failure)
		report.problems.push_back("cannot delete " + path + ": " + failure.message());
	else
		report.removed.push_back(path);
}

/* The removals in progress and abandoned clone temps below root. */
void SweepRoot(const std::string &root, SweepReport &report)
{
	std::error_code failure = WalkRoot(root, [&report](const RootEntry &entry) {
		bool isLeftover = entry.kind == RootEntryKind::Removal ||
		                  (entry.kind == RootEntryKind::CloneTemp &&
		                   IsOlderThan(LastActivityOfClone(entry.path), kStaleTempAge));
		if (isLeftover)
			RemoveLeftover(entry.path, report);
	});
	if (failure && failure != std::errc::no_such_file_or_directory)
		report.problems.push_back("cannot look through " + root + ": " + failure.message());
}

/* An empty bare repository at directory that fetches every branch and tag
 * of the remote into its own. */
Result<RepositoryPtr> InitMirror(const std::string &directory, const Remote &remote)
{
	git_repository *repository = nullptr;
	if (git_repository_init(&repository, directory.c_str(), 1) < 0)
		return GitFail<RepositoryPtr>(Status::Failed, "cannot create " + directory);
	RepositoryPtr owner(repository);

	git_remote *created = nullptr;
	if (git_remote_create_with_fetchspec(&created, repository, kRemoteName, remote.url.c_str(), kBranchRefspec) < 0)
		return GitFail<RepositoryPtr>(Status::Failed, "cannot set the remote of " + directory);
	git_remote_free(created);
	if (git_remote_add_fetch(repository, kRemoteName, kTagRefspec) < 0)
		return GitFail<RepositoryPtr>(Status::Failed, "cannot set the remote of " + directory);
	return Ok(std::move(owner));
}

RepositoryInfo DescribeRepository(git_repository *repository, const std::string &path, const std::string &name)
{
	RepositoryInfo info{name, {}, {}, {}, 0};

	git_remote *remote = nullptr;
	if (git_remote_lookup(&remote, repository, kRemoteName) == 0) {
		RemotePtr remoteOwner(remote);
		if (const char *url = git_remote_url(remote))
			info.url = url;
	}

	RefSnapshot refs = ReadRefs(repository);
	info.headBranch = refs.headBranch;
	info.headSha = refs.headSha;

	struct stat fetchHead;
	if (stat((path + "/FETCH_HEAD").c_str(), &fetchHead) == 0)
		info.fetchTime = static_cast<int64_t>(fetchHead.st_mtime);
	return info;
}

/* Whether the commit changed repoPath against every one of its parents. */
bool ChangesPath(git_commit *commit, const std::string &repoPath)
{
	TreeEntryPtr own = FindEntry(commit, repoPath);
	unsigned parentCount = git_commit_parentcount(commit);
	if (parentCount == 0)
		return own != nullptr;

	for (unsigned i = 0; i < parentCount; i++) {
		git_commit *parent = nullptr;
		if (git_commit_parent(&parent, commit, i) < 0)
			continue;
		CommitPtr parentOwner(parent);
		TreeEntryPtr theirs = FindEntry(parent, repoPath);
		bool isSame = (!own && !theirs) ||
		              (own && theirs && git_oid_equal(git_tree_entry_id(own.get()), git_tree_entry_id(theirs.get())));
		if (isSame)
			return false;
	}
	return true;
}

Commit DescribeCommit(git_commit *commit)
{
	const git_signature *author = git_commit_author(commit);
	const char *summary = git_commit_summary(commit);
	const char *message = git_commit_message(commit);
	return Commit{
		ShaOf(git_commit_id(commit)),
		author && author->name ? author->name : "",
		author && author->email ? author->email : "",
		author ? static_cast<int64_t>(author->when.time) : 0,
		summary ? summary : "",
		message ? message : "",
	};
}

DiffStatus DiffStatusOf(git_delta_t delta)
{
	if (delta == GIT_DELTA_ADDED)
		return DiffStatus::Added;
	if (delta == GIT_DELTA_DELETED)
		return DiffStatus::Deleted;
	return DiffStatus::Modified;
}

} // namespace

std::string Initialize(const Config &config, const std::string &caBundle)
{
	if (git_libgit2_init() < 0)
		return "cannot start libgit2: " + LastGitMessage();

	/* Only the repositories' own settings count: nothing from the server
	 * user's or the system's git configuration, which could rewrite URLs. */
	for (int level : {GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_GLOBAL,
	                  GIT_CONFIG_LEVEL_PROGRAMDATA})
		git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, "");
	/* A shared root may belong to another user. */
	git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, 0);
	git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, config.connectTimeout * 1000);
	git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, config.stallTimeout * 1000);
	/* What a remote sends may not make the indexer allocate beyond the size
	 * cap: no object rebuilt from deltas may be larger than it, and a pack
	 * may not announce more objects than the cap's bytes could hold. */
	size_t maxObjectBytes = static_cast<size_t>(std::min<uint64_t>(config.maxRepositoryBytes, SIZE_MAX));
	git_libgit2_opts(GIT_OPT_SET_PACK_MAX_OBJECT_SIZE, maxObjectBytes);
	git_libgit2_opts(GIT_OPT_SET_PACK_MAX_OBJECTS, maxObjectBytes / kMinPackedObjectBytes);

	if (git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, caBundle.c_str(), nullptr) < 0) {
		std::string reason = "cannot load the CA certificates of " + caBundle + ": " + LastGitMessage();
		git_libgit2_shutdown();
		return reason;
	}
	return {};
}

void Shutdown()
{
	git_libgit2_shutdown();
}

Result<std::string> CanonicalPath(const std::string &path)
{
	std::error_code failure;
	std::string canonical = fs::weakly_canonical(fs::path(path), failure).string();
	if (failure)
		return Fail<std::string>(Status::Failed, "cannot resolve " + path + ": " + failure.message());
	while (canonical.size() > 1 && canonical.back() == '/')
		canonical.pop_back();
	return Ok(canonical);
}

bool IsLockExistsMessage(const std::string &message)
{
	/* libgit2's lock_file raises this one only when the lock file is there
	 * already. */
	if (message.find("failed to lock file") != std::string::npos)
		return true;
	/* This one follows every failed exclusive create of a lock file, with
	 * the reason after it: only "File exists" is another process's lock. */
	return message.find("failed to create locked file") != std::string::npos &&
	       message.find(strerror(EEXIST)) != std::string::npos;
}

bool IsWatchableRef(const std::string &ref)
{
	if (ref == kHeadRef)
		return true;
	if (ref.empty() || StartsWith(ref, "refs/"))
		return false;
	int isValid = 0;
	return git_reference_name_is_valid(&isValid, (kBranchPrefix + ref).c_str()) == 0 && isValid;
}

bool Exists(const std::string &path)
{
	return OpenRepository(path) != nullptr;
}

Result<Nothing> Clone(const std::string &path, const Remote &remote, uint64_t maxBytes,
                      const std::atomic<bool> &abort)
{
	std::error_code failure;
	fs::create_directories(fs::path(path).parent_path(), failure);
	if (failure)
		return Fail<Nothing>(Status::Failed, "cannot create the directory of " + path + ": " + failure.message());

	/* The temp directory is this clone's alone, so it may delete it. */
	Result<std::string> created = CreateCloneTemp(path);
	if (!created.ok())
		return FailLike<Nothing>(created);
	const std::string &temp = created.value;
	Result<Nothing> synced;
	{
		Result<RepositoryPtr> mirror = InitMirror(temp, remote);
		synced = mirror.ok() ? SyncWithRemote(mirror.value.get(), temp, maxBytes, "", abort) : FailLike<Nothing>(mirror);
	}
	if (!synced.ok()) {
		RemoveDirectory(temp);
		return synced;
	}

	if (rename(temp.c_str(), path.c_str()) == 0)
		return {};
	std::string reason = ErrnoText();
	RemoveDirectory(temp);
	if (Exists(path))
		return {};
	return Fail<Nothing>(Status::Failed, "cannot move the clone to " + path + ": " + reason);
}

Result<FetchReport> Fetch(const std::string &path, uint64_t maxBytes, const std::atomic<bool> &abort)
{
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<FetchReport>(path);

	RefSnapshot before = ReadRefs(repository.get());
	/* Nothing ever compacts a repository, so one that reached the cap stays
	 * there: the error tells the way out. */
	Result<Nothing> synced = SyncWithRemote(repository.get(), path, maxBytes, kGrowthAdvice, abort);

	Result<FetchReport> result;
	result.status = synced.status;
	result.error = synced.error;
	result.value.refs = ReadRefs(repository.get());
	result.value.hasRefs = true;
	result.value.changed = !SameRefs(before, result.value.refs);
	return result;
}

Result<std::string> Resolve(const std::string &path, const std::string &rev)
{
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<std::string>(path);

	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<std::string>(commit);
	return Ok(ShaOf(git_commit_id(commit.value.get())));
}

Result<RefSnapshot> ListRefs(const std::string &path)
{
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<RefSnapshot>(path);
	return Ok(ReadRefs(repository.get()));
}

Result<std::vector<Commit>> Log(const std::string &path, const std::string &rev, const std::string &repoPath,
                                int maxCount, int64_t since)
{
	using Commits = std::vector<Commit>;
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<Commits>(path);
	Result<CommitPtr> start = ResolveCommit(repository.get(), rev);
	if (!start.ok())
		return FailLike<Commits>(start);

	git_revwalk *walk = nullptr;
	if (git_revwalk_new(&walk, repository.get()) < 0)
		return GitFail<Commits>(Status::Failed, "cannot walk the history");
	RevwalkPtr walkOwner(walk);
	git_revwalk_sorting(walk, GIT_SORT_TIME);
	if (git_revwalk_push(walk, git_commit_id(start.value.get())) < 0)
		return GitFail<Commits>(Status::Failed, "cannot walk the history");

	Commits commits;
	git_oid id;
	while (static_cast<int>(commits.size()) < maxCount && git_revwalk_next(&id, walk) == 0) {
		git_commit *next = nullptr;
		if (git_commit_lookup(&next, repository.get(), &id) < 0)
			return GitFail<Commits>(Status::Failed, "cannot read the commit " + ShaOf(&id));
		CommitPtr commit(next);
		/* The walk goes by commit time, newest first, so every commit after
		 * this one was committed about as early or earlier. */
		if (static_cast<int64_t>(git_commit_time(next)) < since - kLogSkewTolerance)
			break;
		if (git_commit_author(next)->when.time < since)
			continue;
		if (!repoPath.empty() && !ChangesPath(next, repoPath))
			continue;
		commits.push_back(DescribeCommit(next));
	}
	return Ok(std::move(commits));
}

Result<std::vector<DiffEntry>> Diff(const std::string &path, const std::string &baseRev,
                                    const std::string &headRev, const std::string &repoPath)
{
	using Entries = std::vector<DiffEntry>;
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<Entries>(path);
	Result<CommitPtr> base = ResolveCommit(repository.get(), baseRev);
	if (!base.ok())
		return FailLike<Entries>(base);
	Result<CommitPtr> head = ResolveCommit(repository.get(), headRev);
	if (!head.ok())
		return FailLike<Entries>(head);

	git_tree *baseTree = nullptr;
	git_tree *headTree = nullptr;
	if (git_commit_tree(&baseTree, base.value.get()) < 0)
		return GitFail<Entries>(Status::Failed, "cannot read the tree of \"" + baseRev + "\"");
	TreePtr baseOwner(baseTree);
	if (git_commit_tree(&headTree, head.value.get()) < 0)
		return GitFail<Entries>(Status::Failed, "cannot read the tree of \"" + headRev + "\"");
	TreePtr headOwner(headTree);

	git_diff_options options = GIT_DIFF_OPTIONS_INIT;
	options.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH;
	char *prefix = const_cast<char *>(repoPath.c_str());
	if (!repoPath.empty()) {
		options.pathspec.strings = &prefix;
		options.pathspec.count = 1;
	}
	git_diff *diff = nullptr;
	if (git_diff_tree_to_tree(&diff, repository.get(), baseTree, headTree, &options) < 0)
		return GitFail<Entries>(Status::Failed, "cannot compare the revisions");
	DiffPtr diffOwner(diff);

	Entries entries;
	size_t count = git_diff_num_deltas(diff);
	for (size_t i = 0; i < count; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		DiffStatus status = DiffStatusOf(delta->status);
		const char *file = status == DiffStatus::Deleted ? delta->old_file.path : delta->new_file.path;
		entries.push_back(DiffEntry{file, status, ShaOf(&delta->old_file.id), ShaOf(&delta->new_file.id)});
	}
	return Ok(std::move(entries));
}

Result<std::vector<TreeEntry>> ListTree(const std::string &path, const std::string &rev,
                                        const std::string &repoPath, bool recursive)
{
	using Entries = std::vector<TreeEntry>;
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<Entries>(path);
	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<Entries>(commit);
	return ListDirectory(repository.get(), commit.value.get(), repoPath, recursive);
}

Result<std::string> ReadFile(const std::string &path, const std::string &rev, const std::string &repoPath,
                             uint64_t maxBytes)
{
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<std::string>(path);
	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<std::string>(commit);
	Result<TreeEntryPtr> entry = FindFile(commit.value.get(), repoPath);
	if (!entry.ok())
		return FailLike<std::string>(entry);

	const git_oid *id = git_tree_entry_id(entry.value.get());
	Result<uint64_t> size = BlobSize(repository.get(), id);
	if (!size.ok())
		return FailLike<std::string>(size);
	if (size.value > maxBytes)
		return Fail<std::string>(Status::TooLarge, "\"" + repoPath + "\" has " + std::to_string(size.value) +
		                                           " bytes, more than the " + std::to_string(maxBytes) + " asked for");

	git_blob *blob = nullptr;
	if (git_blob_lookup(&blob, repository.get(), id) < 0)
		return GitFail<std::string>(Status::Failed, "cannot read the file \"" + repoPath + "\"");
	BlobPtr blobOwner(blob);
	return Ok(std::string(static_cast<const char *>(git_blob_rawcontent(blob)),
	                      static_cast<size_t>(git_blob_rawsize(blob))));
}

Result<std::vector<Change>> WriteFile(const std::string &path, const std::string &rev,
                                      const std::string &repoPath, const Destination &destination,
                                      const WritePolicy &policy)
{
	using Changes = std::vector<Change>;
	if (HasWriteTempName(destination.display))
		return FailLike<Changes>(RefuseWriteTempName(destination.display));
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<Changes>(path);
	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<Changes>(commit);
	Result<TreeEntryPtr> entry = FindFile(commit.value.get(), repoPath);
	if (!entry.ok())
		return FailLike<Changes>(entry);

	const git_oid *id = git_tree_entry_id(entry.value.get());
	Result<uint64_t> size = BlobSize(repository.get(), id);
	if (!size.ok())
		return FailLike<Changes>(size);
	TreeEntry source{repoPath, EntryType::File, size.value, ShaOf(id),
	                 git_tree_entry_filemode(entry.value.get()) == GIT_FILEMODE_BLOB_EXECUTABLE};

	Changes changes;
	Result<Nothing> synced = SyncFile(repository.get(), source, destination, policy, changes);
	if (!synced.ok())
		return FailWithChanges(synced, std::move(changes));
	return Ok(std::move(changes));
}

Result<std::vector<Change>> WriteDir(const std::string &path, const std::string &rev,
                                     const std::string &repoPath, const Destination &destination,
                                     const std::string &oldRev, const WritePolicy &policy)
{
	using Changes = std::vector<Change>;
	using Files = std::map<std::string, TreeEntry>;
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<Changes>(path);
	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<Changes>(commit);
	Result<Files> files = FilesBelow(repository.get(), commit.value.get(), repoPath);
	if (!files.ok())
		return FailLike<Changes>(files);

	Files oldFiles;
	if (!oldRev.empty()) {
		Result<CommitPtr> oldCommit = ResolveCommit(repository.get(), oldRev);
		if (!oldCommit.ok())
			return FailLike<Changes>(oldCommit);
		Result<Files> found = FilesBelow(repository.get(), oldCommit.value.get(), repoPath);
		if (!found.ok() && found.status != Status::NotFound)
			return FailLike<Changes>(found);
		oldFiles = std::move(found.value);
	}

	if (HasWriteTempName(destination.display))
		return FailLike<Changes>(RefuseWriteTempName(destination.display));
	for (const Files *named : {&files.value, &oldFiles}) {
		for (const auto &[relativePath, entry] : *named) {
			if (HasWriteTempName(relativePath))
				return FailLike<Changes>(RefuseWriteTempName(entry.path));
		}
	}

	uint64_t totalBytes = 0;
	for (const auto &[relativePath, entry] : files.value)
		totalBytes += entry.size;
	if (totalBytes > policy.maxBytes)
		return Fail<Changes>(Status::TooLarge, "the files of \"" + repoPath + "\" have " + std::to_string(totalBytes) +
		                                       " bytes together, more than a write-out takes");

	Changes changes;
	for (const auto &[relativePath, entry] : oldFiles) {
		if (files.value.count(relativePath))
			continue;
		Destination gone = Below(destination, relativePath);
		Result<Nothing> allowed = CheckDestination(gone, policy);
		if (!allowed.ok())
			return FailWithChanges(allowed, std::move(changes));
		/* The temp file of a write-out of it that a crash cut short goes
		 * with it; no later write-out of the file would replace it. */
		unlink((gone.absolute + kWriteTempSuffix).c_str());
		if (!FileExists(gone.absolute))
			continue;
		if (unlink(gone.absolute.c_str()) != 0)
			return FailWithChanges(Fail<Nothing>(Status::Failed, "cannot delete " + gone.absolute + ": " + ErrnoText()),
			                       std::move(changes));
		changes.push_back(Change{gone.display, ChangeKind::Deleted});
	}
	for (const auto &[relativePath, entry] : files.value) {
		Result<Nothing> synced = SyncFile(repository.get(), entry, Below(destination, relativePath), policy, changes);
		if (!synced.ok())
			return FailWithChanges(synced, std::move(changes));
	}
	return Ok(std::move(changes));
}

Result<FileState> CompareFile(const std::string &path, const std::string &rev, const std::string &repoPath,
                              const Destination &localFile, const WritePolicy &policy)
{
	Result<Nothing> allowed = CheckDestination(localFile, policy);
	if (!allowed.ok())
		return FailLike<FileState>(allowed);
	RepositoryPtr repository = OpenRepository(path);
	if (!repository)
		return NotCloned<FileState>(path);
	Result<CommitPtr> commit = ResolveCommit(repository.get(), rev);
	if (!commit.ok())
		return FailLike<FileState>(commit);
	Result<TreeEntryPtr> entry = FindFile(commit.value.get(), repoPath);
	if (!entry.ok())
		return FailLike<FileState>(entry);

	const git_oid *id = git_tree_entry_id(entry.value.get());
	Result<uint64_t> size = BlobSize(repository.get(), id);
	if (!size.ok())
		return FailLike<FileState>(size);
	if (!FileExists(localFile.absolute))
		return Ok(FileState::Missing);
	return Ok(HasContentOf(localFile.absolute, id, size.value) ? FileState::Same : FileState::Different);
}

Result<Nothing> Remove(const std::string &path)
{
	if (!Exists(path))
		return NotCloned<Nothing>(path);

	/* Into a directory of its own first: the rename is atomic, so every
	 * reader sees the repository whole or not at all, and an interrupted
	 * deletion leaves only a directory the next sweep takes. */
	Result<std::string> doomed = CreateRemovalDirectory(path);
	if (!doomed.ok())
		return FailLike<Nothing>(doomed);
	std::string moved = RemovedRepositoryIn(doomed.value);
	if (rename(path.c_str(), moved.c_str()) != 0) {
		int failure = errno;
		rmdir(doomed.value.c_str());
		if (failure == ENOENT)
			return NotCloned<Nothing>(path);
		return Fail<Nothing>(Status::Failed, "cannot move " + path + " out of the way: " + strerror(failure));
	}

	/* Another server's sweep may delete it at the same time. */
	std::error_code failure;
	fs::remove_all(doomed.value, failure);
	std::error_code ignored;
	if (failure && fs::exists(doomed.value, ignored))
		return Fail<Nothing>(Status::Failed, "the repository is gone, but not all of " + doomed.value +
		                                     " could be deleted (" + failure.message() +
		                                     "); the rest is deleted when the extension loads next");
	return {};
}

Result<std::vector<RepositoryInfo>> ListRepositories(const std::string &root)
{
	using Repositories = std::vector<RepositoryInfo>;
	Repositories repositories;
	std::error_code failure;
	/* Opened once without skipping: an unreadable root is a failure, not an
	 * empty one. Below it, directories that cannot be read are skipped. */
	fs::directory_iterator top(root, failure);
	if (failure == std::errc::no_such_file_or_directory)
		return Ok(std::move(repositories));
	if (failure)
		return Fail<Repositories>(Status::Failed, "cannot read the root " + root + ": " + failure.message());

	failure = WalkRoot(root, [&root, &repositories](const RootEntry &entry) {
		if (entry.kind != RootEntryKind::Repository)
			return;
		RepositoryPtr repository = OpenRepository(entry.path);
		if (repository)
			repositories.push_back(
				DescribeRepository(repository.get(), entry.path, RepositoryNameAt(root, entry.path)));
	});
	if (failure)
		return Fail<Repositories>(Status::Failed, "cannot read below the root " + root + ": " + failure.message());
	return Ok(std::move(repositories));
}

std::vector<std::string> RemoveStaleLocks(const std::string &path, std::chrono::seconds maxAge)
{
	std::vector<std::string> removed;
	auto removeWhenStale = [&](const fs::path &file) {
		std::string name = file.string();
		time_t modified = ModifiedTime(name);
		if (!EndsWith(name, kLockSuffix) || !IsOlderThan(modified, maxAge) || unlink(name.c_str()) != 0)
			return;
		removed.push_back(name);
		std::string minutes = std::to_string((time(nullptr) - modified) / 60);
		NoteForLog(LogLevel::Message, "Deleted the stale lock file ",
		           (name + ", unchanged for " + minutes + " minutes").c_str());
	};

	std::error_code failure;
	for (fs::directory_iterator it(path, failure), end; !failure && it != end; it.increment(failure)) {
		std::error_code typeFailure;
		if (it->is_regular_file(typeFailure))
			removeWhenStale(it->path());
	}
	fs::recursive_directory_iterator refs(path + "/refs", fs::directory_options::skip_permission_denied, failure), end;
	for (; !failure && refs != end; refs.increment(failure)) {
		std::error_code typeFailure;
		if (refs->is_regular_file(typeFailure))
			removeWhenStale(refs->path());
	}
	return removed;
}

SweepReport SweepLeftovers(const std::string &root)
{
	SweepReport report;
	SweepRoot(root, report);
	return report;
}

} // namespace smgit

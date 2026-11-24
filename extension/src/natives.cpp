/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The natives of scripting/include/git.inc. Everything a plugin passes is
 * parsed here, so the git work only ever sees valid values. Work natives
 * queue a task and answer through the plugin's callback, as
 * (status, payload..., error, data); the getters of handles answer at once
 * from memory. */
#include "extension.h"

#include <limits.h>
#include <string.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

using smgit::Completion;
using smgit::Result;
using smgit::Status;

namespace {

const int kMaxLogCount = 1000;

/* A string argument; none, with a native error, when its address is not
 * in the plugin's memory. */
std::optional<std::string> ReadString(IPluginContext *context, cell_t address, const char *argumentName)
{
	char *text = nullptr;
	if (context->LocalToString(address, &text) != SP_ERROR_NONE || !text) {
		context->ReportError("%s is no string of the plugin (address %x)", argumentName, address);
		return std::nullopt;
	}
	return std::string(text);
}

/* A revision argument: anything but nothing. */
bool ReadRevision(IPluginContext *context, cell_t address, const char *argumentName, std::string &rev)
{
	std::optional<std::string> given = ReadString(context, address, argumentName);
	if (!given)
		return false;
	rev = *given;
	if (!rev.empty())
		return true;
	context->ReportError("%s must name a revision", argumentName);
	return false;
}

/* A path inside the repository; with mustNameSomething not its top. */
bool ReadRepoPath(IPluginContext *context, cell_t address, const char *argumentName, bool mustNameSomething,
                  std::string &repoPath)
{
	std::optional<std::string> read = ReadString(context, address, argumentName);
	if (!read)
		return false;
	const std::string &given = *read;
	std::optional<std::string> parsed = smgit::ParseRepoPath(given);
	if (!parsed || (mustNameSomething && parsed->empty())) {
		context->ReportError("%s \"%s\" must be a path inside the repository, without empty, \".\", \"..\" or "
		                     "\".git\" parts", argumentName, given.c_str());
		return false;
	}
	repoPath = *parsed;
	return true;
}

/* A path below the game directory, as the plugin named it and as the
 * absolute path below the canonical game directory. Built here rather than
 * with SourceMod's BuildPath, which would turn backslashes into slashes
 * after the parsing. */
bool ReadGamePath(IPluginContext *context, cell_t address, const char *argumentName, smgit::Destination &place)
{
	std::optional<std::string> read = ReadString(context, address, argumentName);
	if (!read)
		return false;
	const std::string &given = *read;
	std::optional<std::string> parsed = smgit::ParseGamePath(given);
	std::optional<std::string> absolute =
		parsed ? smgit::PathBelow(g_Git.GameDirectory(), *parsed) : std::optional<std::string>();
	if (!absolute) {
		context->ReportError("%s \"%s\" must be a non-empty path relative to the game directory, without empty, "
		                     "\".\", \"..\" or \".git\" parts and without backslashes",
		                     argumentName, given.c_str());
		return false;
	}
	place = smgit::Destination{*absolute, *parsed};
	return true;
}

cell_t WriteString(IPluginContext *context, const cell_t *params, int bufferParam, const std::string &text)
{
	size_t written = 0;
	context->StringToLocalUTF8(params[bufferParam], params[bufferParam + 1], text.c_str(), &written);
	return static_cast<cell_t>(written);
}

/* Queues work on a worker and has its result delivered to the plugin's
 * callback; pushPayload pushes what goes between status and error. */
template <typename Work, typename Push>
cell_t Queue(smgit::Worker &worker, IPluginContext *context, cell_t function, cell_t data, Work work,
             Push pushPayload)
{
	CallbackId id;
	if (!g_Git.Expect(context, static_cast<funcid_t>(function), data, id))
		return context->ThrowNativeError("Invalid callback function %x", function);

	worker.Submit([id, work = std::move(work), pushPayload = std::move(pushPayload)](
	                      const std::atomic<bool> &abort) -> Completion {
		auto result = smgit::Guarded([&] { return work(abort); });
		return [id, result = std::move(result), pushPayload] {
			g_Git.Deliver(id, result.status, result.error,
			              [&](IPluginFunction *function, IPluginContext *plugin) {
				              pushPayload(function, plugin, result);
			              });
		};
	});
	return 0;
}

/* The payload of callbacks that get a result handle: the handle when the
 * work succeeded, otherwise none. */
struct PushResultHandle {
	template <typename T>
	void operator()(IPluginFunction *function, IPluginContext *plugin, const Result<T> &result) const
	{
		function->PushCell(result.ok() ? g_Git.CreateResultHandle(plugin, ResultData(result.value)) : BAD_HANDLE);
	}
};

/* The payload of write-outs: the changes they made, also when they failed
 * partway, so the plugin always gets a handle to delete. */
struct PushChangesHandle {
	void operator()(IPluginFunction *function, IPluginContext *plugin,
	                const Result<std::vector<smgit::Change>> &written) const
	{
		function->PushCell(g_Git.CreateResultHandle(plugin, ResultData(written.value)));
	}
};

/* Finishes GitRepo.Open: the plugin gets its handle. */
void DeliverOpened(CallbackId id, const RepoRef &repo, const Result<smgit::Nothing> &opened, bool wasCloned)
{
	g_Git.EndOpen(id);
	g_Git.Deliver(id, opened.status, opened.error, [&](IPluginFunction *function, IPluginContext *plugin) {
		function->PushCell(opened.ok() ? g_Git.CreateRepoHandle(plugin, repo) : BAD_HANDLE);
		function->PushCell(opened.ok() && wasCloned);
	});
}

/* Opens the repository on the network worker, behind the clones, fetches
 * and removals queued before: it is there, or it is cloned when mayClone.
 * Until it is answered, the plugin counts as a user of the repository, so
 * a removal checked meanwhile is refused instead of deleting what the
 * plugin is about to get. */
void QueueOpenOnNetwork(CallbackId id, const RepoRef &repo, bool mayClone, const std::string &pluginName)
{
	g_Git.BeginOpen(id, repo->remote.name, pluginName);
	uint64_t maxBytes = g_Git.Settings().maxRepositoryBytes;
	g_Git.Network().Submit([id, repo, mayClone, maxBytes](const std::atomic<bool> &abort) -> Completion {
		bool isThere = false;
		Result<smgit::Nothing> opened = smgit::Guarded([&] {
			isThere = smgit::Exists(repo->path);
			if (isThere)
				return Result<smgit::Nothing>{};
			if (!mayClone)
				return smgit::Fail<smgit::Nothing>(Status::NotCloned, repo->remote.name + " is not cloned");
			return smgit::Clone(repo->path, repo->remote, maxBytes, abort);
		});
		return [id, repo, opened, isThere] { DeliverOpened(id, repo, opened, !isThere); };
	});
}

// GitRepo.Open(url, callback, data, clone)
cell_t GitRepo_Open(IPluginContext *context, const cell_t *params)
{
	/* Plugins compiled before clone existed pass three arguments. */
	bool mayClone = params[0] < 4 || params[4] != 0;
	std::optional<std::string> url = ReadString(context, params[1], "url");
	if (!url)
		return 0;
	CallbackId id;
	if (!g_Git.Expect(context, static_cast<funcid_t>(params[2]), params[3], id))
		return context->ThrowNativeError("Invalid callback function %x", params[2]);

	std::string refusal;
	std::optional<smgit::Remote> remote = smgit::ParseRemoteUrl(*url, refusal);
	if (!remote) {
		g_Git.Local().Submit([id, refusal](const std::atomic<bool> &) -> Completion {
			return [id, refusal] {
				Result<smgit::Nothing> refused;
				refused.status = Status::Refused;
				refused.error = refusal;
				DeliverOpened(id, nullptr, refused, false);
			};
		});
		return 0;
	}

	/* A removal that runs at any time from now until the local look at the
	 * disk was answered may delete the repository after that look: the open
	 * then asks again on the network worker, behind the removal, so no
	 * handle is given out on a repository about to go or gone. */
	RepoRef repo = g_Git.RepositoryOf(*remote);
	std::string pluginName = g_Git.NameOfPlugin(context);
	uint64_t removalsBefore = g_Git.RemovalsStartedOf(repo->remote.name);
	bool wasBeingRemoved = g_Git.IsBeingRemoved(repo->remote.name);
	g_Git.Local().Submit([id, repo, mayClone, pluginName, removalsBefore,
	                      wasBeingRemoved](const std::atomic<bool> &) -> Completion {
		Result<bool> found = smgit::Guarded([&] { return smgit::Ok(smgit::Exists(repo->path)); });
		return [id, repo, mayClone, pluginName, removalsBefore, wasBeingRemoved, found] {
			const std::string &name = repo->remote.name;
			bool mayBeRemoved = wasBeingRemoved || g_Git.IsBeingRemoved(name) ||
			                    g_Git.RemovalsStartedOf(name) != removalsBefore;
			if (!found.ok())
				DeliverOpened(id, repo, smgit::FailLike<smgit::Nothing>(found), false);
			else if (mayBeRemoved)
				QueueOpenOnNetwork(id, repo, mayClone, pluginName);
			else if (found.value)
				DeliverOpened(id, repo, {}, false);
			else if (!mayClone)
				DeliverOpened(id, repo,
				              smgit::Fail<smgit::Nothing>(Status::NotCloned, repo->remote.name + " is not cloned"),
				              false);
			else
				QueueOpenOnNetwork(id, repo, true, pluginName);
		};
	});
	return 0;
}

// GitRepo.Fetch(callback, data)
cell_t GitRepo_Fetch(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	if (!handle)
		return 0;
	CallbackId id;
	if (!g_Git.Expect(context, static_cast<funcid_t>(params[2]), params[3], id))
		return context->ThrowNativeError("Invalid callback function %x", params[2]);

	RepoRef repo = handle->repo;
	uint64_t maxBytes = g_Git.Settings().maxRepositoryBytes;
	g_Git.Network().Submit([id, repo, maxBytes](const std::atomic<bool> &abort) -> Completion {
		Result<smgit::FetchReport> fetched = smgit::Guarded([&] { return smgit::Fetch(repo->path, maxBytes, abort); });
		return [id, repo, fetched] {
			if (fetched.value.hasRefs)
				g_Git.ObserveRefs(repo->remote.name, fetched.value.refs);
			g_Git.Deliver(id, fetched.status, fetched.error, [&](IPluginFunction *function, IPluginContext *) {
				function->PushCell(fetched.ok() && fetched.value.changed);
			});
		};
	});
	return 0;
}

// GitRepo.Resolve(rev, callback, data)
cell_t GitRepo_Resolve(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev;
	if (!handle || !ReadRevision(context, params[2], "rev", rev))
		return 0;

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[3], params[4],
	             [path, rev](const std::atomic<bool> &) { return smgit::Resolve(path, rev); },
	             [](IPluginFunction *function, IPluginContext *, const Result<std::string> &resolved) {
		             function->PushString(resolved.value.c_str());
	             });
}

// GitRepo.ListRefs(callback, data)
cell_t GitRepo_ListRefs(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	if (!handle)
		return 0;

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[2], params[3],
	             [path](const std::atomic<bool> &) { return smgit::ListRefs(path); }, PushResultHandle());
}

// GitRepo.Log(rev, callback, data, path, max, since)
cell_t GitRepo_Log(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[5], "path", false, repoPath))
		return 0;
	int maxCount = params[6];
	if (maxCount < 1 || maxCount > kMaxLogCount)
		return context->ThrowNativeError("max %d is outside 1-%d", maxCount, kMaxLogCount);
	int64_t since = params[7];

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[3], params[4],
	             [path, rev, repoPath, maxCount, since](const std::atomic<bool> &) {
		             return smgit::Log(path, rev, repoPath, maxCount, since);
	             },
	             PushResultHandle());
}

// GitRepo.Diff(base, head, callback, data, path)
cell_t GitRepo_Diff(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string base, head, repoPath;
	if (!handle || !ReadRevision(context, params[2], "base", base) ||
	    !ReadRevision(context, params[3], "head", head) || !ReadRepoPath(context, params[6], "path", false, repoPath))
		return 0;

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[4], params[5],
	             [path, base, head, repoPath](const std::atomic<bool> &) {
		             return smgit::Diff(path, base, head, repoPath);
	             },
	             PushResultHandle());
}

// GitRepo.ListTree(rev, callback, data, path, recursive)
cell_t GitRepo_ListTree(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[5], "path", false, repoPath))
		return 0;
	bool recursive = params[6] != 0;

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[3], params[4],
	             [path, rev, repoPath, recursive](const std::atomic<bool> &) {
		             return smgit::ListTree(path, rev, repoPath, recursive);
	             },
	             PushResultHandle());
}

// GitRepo.ReadFile(rev, path, callback, data, maxBytes)
cell_t GitRepo_ReadFile(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[3], "path", true, repoPath))
		return 0;
	if (params[6] < 1)
		return context->ThrowNativeError("maxBytes %d must be positive", params[6]);
	uint64_t maxBytes = static_cast<uint64_t>(params[6]);

	std::string path = handle->repo->path;
	return Queue(g_Git.Local(), context, params[4], params[5],
	             [path, rev, repoPath, maxBytes](const std::atomic<bool> &) {
		             return smgit::ReadFile(path, rev, repoPath, maxBytes);
	             },
	             [](IPluginFunction *function, IPluginContext *, const Result<std::string> &read) {
		             function->PushStringEx(const_cast<char *>(read.value.c_str()), read.value.size() + 1,
		                                    SM_PARAM_STRING_COPY | SM_PARAM_STRING_BINARY, 0);
		             function->PushCell(static_cast<cell_t>(read.value.size()));
	             });
}

// GitRepo.WriteFile(rev, path, dest, callback, data)
cell_t GitRepo_WriteFile(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	smgit::Destination destination;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[3], "path", true, repoPath) ||
	    !ReadGamePath(context, params[4], "dest", destination))
		return 0;

	std::string path = handle->repo->path;
	smgit::WritePolicy policy = g_Git.WriteLimits();
	return Queue(g_Git.Local(), context, params[5], params[6],
	             [path, rev, repoPath, destination, policy](const std::atomic<bool> &) {
		             return smgit::WriteFile(path, rev, repoPath, destination, policy);
	             },
	             PushChangesHandle());
}

// GitRepo.WriteDir(rev, path, destDir, callback, data, oldRev)
cell_t GitRepo_WriteDir(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	smgit::Destination destination;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[3], "path", false, repoPath) ||
	    !ReadGamePath(context, params[4], "destDir", destination))
		return 0;
	std::optional<std::string> oldRevGiven = ReadString(context, params[7], "oldRev");
	if (!oldRevGiven)
		return 0;
	std::string oldRev = *oldRevGiven;

	std::string path = handle->repo->path;
	smgit::WritePolicy policy = g_Git.WriteLimits();
	return Queue(g_Git.Local(), context, params[5], params[6],
	             [path, rev, repoPath, destination, oldRev, policy](const std::atomic<bool> &) {
		             return smgit::WriteDir(path, rev, repoPath, destination, oldRev, policy);
	             },
	             PushChangesHandle());
}

// GitRepo.CompareFile(rev, path, file, callback, data)
cell_t GitRepo_CompareFile(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	std::string rev, repoPath;
	smgit::Destination file;
	if (!handle || !ReadRevision(context, params[2], "rev", rev) ||
	    !ReadRepoPath(context, params[3], "path", true, repoPath) || !ReadGamePath(context, params[4], "file", file))
		return 0;

	std::string path = handle->repo->path;
	smgit::WritePolicy policy = g_Git.WriteLimits();
	return Queue(g_Git.Local(), context, params[5], params[6],
	             [path, rev, repoPath, file, policy](const std::atomic<bool> &) {
		             return smgit::CompareFile(path, rev, repoPath, file, policy);
	             },
	             [](IPluginFunction *function, IPluginContext *, const Result<smgit::FileState> &compared) {
		             /* A plugin that skips the status check must not take a
		              * failure for "up to date". */
		             smgit::FileState state = compared.ok() ? compared.value : smgit::FileState::Different;
		             function->PushCell(static_cast<cell_t>(state));
	             });
}

// GitRepo.Watch(ref, interval)
cell_t GitRepo_Watch(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	if (!handle)
		return 0;
	std::optional<std::string> ref = ReadString(context, params[2], "ref");
	if (!ref)
		return 0;
	if (!smgit::IsWatchableRef(*ref))
		return context->ThrowNativeError("ref \"%s\" must be HEAD or the short name of a branch or tag", ref->c_str());
	if (params[3] < 0)
		return context->ThrowNativeError("interval %d must not be negative", params[3]);
	g_Git.Watch(context, handle->repo, *ref, params[3]);
	return 0;
}

// GitRepo.Unwatch(ref)
cell_t GitRepo_Unwatch(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	if (!handle)
		return 0;
	std::optional<std::string> ref = ReadString(context, params[2], "ref");
	if (ref)
		g_Git.Unwatch(context, handle->repo->remote.name, *ref);
	return 0;
}

// GitRepo.Remove(callback, data)
cell_t GitRepo_Remove(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	if (!handle)
		return 0;
	CallbackId id;
	if (!g_Git.Expect(context, static_cast<funcid_t>(params[2]), params[3], id))
		return context->ThrowNativeError("Invalid callback function %x", params[2]);

	/* The removal waits behind the local work queued so far, and who uses
	 * the repository is asked then. The deletion itself waits behind the
	 * network work queued so far; from the check until it ended, opens of
	 * the repository wait for it (GitRepo_Open), so nobody gets a handle
	 * on it in between. */
	RepoRef repo = handle->repo;
	uint64_t ownHandle = handle->id;
	g_Git.Local().Submit([id, repo, ownHandle](const std::atomic<bool> &) -> Completion {
		return [id, repo, ownHandle] {
			std::string users = g_Git.UsersOf(repo->remote.name, ownHandle);
			if (!users.empty()) {
				g_Git.Deliver(id, Status::InUse, "in use by " + users, [](IPluginFunction *, IPluginContext *) {});
				return;
			}
			g_Git.BeginRemoval(repo->remote.name);
			g_Git.Network().Submit([id, repo](const std::atomic<bool> &) -> Completion {
				Result<smgit::Nothing> removed = smgit::Guarded([&] { return smgit::Remove(repo->path); });
				return [id, repo, removed] {
					g_Git.EndRemoval(repo->remote.name);
					g_Git.Deliver(id, removed.status, removed.error, [](IPluginFunction *, IPluginContext *) {});
				};
			});
		};
	});
	return 0;
}

// GitRepo.GetUrl(buffer, maxlength)
cell_t GitRepo_GetUrl(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	return handle ? WriteString(context, params, 2, handle->repo->remote.url) : 0;
}

// GitRepo.GetName(buffer, maxlength)
cell_t GitRepo_GetName(IPluginContext *context, const cell_t *params)
{
	RepoHandle *handle = g_Git.ReadRepoHandle(context, params[1]);
	return handle ? WriteString(context, params, 2, handle->repo->remote.name) : 0;
}

// Git_ListRepositories(callback, data)
cell_t Git_ListRepositories(IPluginContext *context, const cell_t *params)
{
	std::string root = g_Git.Root();
	return Queue(g_Git.Local(), context, params[1], params[2],
	             [root](const std::atomic<bool> &) { return smgit::ListRepositories(root); },
	             [](IPluginFunction *function, IPluginContext *plugin,
	                const Result<std::vector<smgit::RepositoryInfo>> &listed) {
		             std::vector<RepositoryRow> rows;
		             for (const smgit::RepositoryInfo &info : listed.value)
			             rows.push_back(RepositoryRow{info, g_Git.OpenersOf(info.name),
			                                          g_Git.WatchesOf(info.name)});
		             function->PushCell(listed.ok() ? g_Git.CreateResultHandle(plugin, ResultData(std::move(rows)))
		                                            : BAD_HANDLE);
	             });
}

/* The rows of a result handle of one methodmap; null with a native error
 * thrown when the handle is of another one. */
template <typename Row>
const std::vector<Row> *ReadRows(IPluginContext *context, cell_t handle)
{
	const ResultData *data = g_Git.ReadResultHandle(context, handle);
	if (!data)
		return nullptr;
	if constexpr (std::is_same_v<Row, smgit::Ref>) {
		if (const smgit::RefSnapshot *snapshot = std::get_if<smgit::RefSnapshot>(data))
			return &snapshot->refs;
	} else {
		if (const std::vector<Row> *rows = std::get_if<std::vector<Row>>(data))
			return rows;
	}
	context->ReportError("Handle %x is a git result of another kind", handle);
	return nullptr;
}

/* The row params[2] of the result handle params[1]. */
template <typename Row>
const Row *ReadRow(IPluginContext *context, const cell_t *params)
{
	const std::vector<Row> *rows = ReadRows<Row>(context, params[1]);
	if (!rows)
		return nullptr;
	if (params[2] < 0 || static_cast<size_t>(params[2]) >= rows->size()) {
		context->ReportError("Index %d is outside the result's %d entries", params[2], static_cast<int>(rows->size()));
		return nullptr;
	}
	return &(*rows)[params[2]];
}

template <typename Row>
cell_t LengthOf(IPluginContext *context, const cell_t *params)
{
	const std::vector<Row> *rows = ReadRows<Row>(context, params[1]);
	return rows ? static_cast<cell_t>(rows->size()) : 0;
}

/* A getter native (handle, index, buffer, maxlength) for a string of a row. */
template <typename Row, std::string Row::*Field>
cell_t StringOf(IPluginContext *context, const cell_t *params)
{
	const Row *row = ReadRow<Row>(context, params);
	return row ? WriteString(context, params, 3, row->*Field) : 0;
}

cell_t ClampToCell(uint64_t value)
{
	return value > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<cell_t>(value);
}

cell_t GitRefList_IsTag(IPluginContext *context, const cell_t *params)
{
	const smgit::Ref *ref = ReadRow<smgit::Ref>(context, params);
	return ref ? ref->isTag : 0;
}

const smgit::RefSnapshot *ReadSnapshot(IPluginContext *context, cell_t handle)
{
	const ResultData *data = g_Git.ReadResultHandle(context, handle);
	if (!data)
		return nullptr;
	const smgit::RefSnapshot *snapshot = std::get_if<smgit::RefSnapshot>(data);
	if (!snapshot)
		context->ReportError("Handle %x is no GitRefList", handle);
	return snapshot;
}

cell_t GitRefList_GetHeadBranch(IPluginContext *context, const cell_t *params)
{
	const smgit::RefSnapshot *snapshot = ReadSnapshot(context, params[1]);
	return snapshot ? WriteString(context, params, 2, snapshot->headBranch) : 0;
}

cell_t GitRefList_GetHeadSha(IPluginContext *context, const cell_t *params)
{
	const smgit::RefSnapshot *snapshot = ReadSnapshot(context, params[1]);
	return snapshot ? WriteString(context, params, 2, snapshot->headSha) : 0;
}

cell_t GitCommitList_GetTime(IPluginContext *context, const cell_t *params)
{
	const smgit::Commit *commit = ReadRow<smgit::Commit>(context, params);
	return commit ? static_cast<cell_t>(commit->time) : 0;
}

cell_t GitDiff_GetStatus(IPluginContext *context, const cell_t *params)
{
	const smgit::DiffEntry *entry = ReadRow<smgit::DiffEntry>(context, params);
	return entry ? static_cast<cell_t>(entry->status) : 0;
}

cell_t GitTree_GetType(IPluginContext *context, const cell_t *params)
{
	const smgit::TreeEntry *entry = ReadRow<smgit::TreeEntry>(context, params);
	return entry ? static_cast<cell_t>(entry->type) : 0;
}

cell_t GitTree_GetSize(IPluginContext *context, const cell_t *params)
{
	const smgit::TreeEntry *entry = ReadRow<smgit::TreeEntry>(context, params);
	return entry ? ClampToCell(entry->size) : 0;
}

cell_t GitTree_IsExecutable(IPluginContext *context, const cell_t *params)
{
	const smgit::TreeEntry *entry = ReadRow<smgit::TreeEntry>(context, params);
	return entry ? entry->isExecutable : 0;
}

cell_t GitChanges_GetKind(IPluginContext *context, const cell_t *params)
{
	const smgit::Change *change = ReadRow<smgit::Change>(context, params);
	return change ? static_cast<cell_t>(change->kind) : 0;
}

/* A getter native (handle, index, buffer, maxlength) for a string of a
 * repository row's info. */
template <std::string smgit::RepositoryInfo::*Field>
cell_t RepositoryStringOf(IPluginContext *context, const cell_t *params)
{
	const RepositoryRow *row = ReadRow<RepositoryRow>(context, params);
	return row ? WriteString(context, params, 3, row->info.*Field) : 0;
}

cell_t GitRepoList_GetFetchTime(IPluginContext *context, const cell_t *params)
{
	const RepositoryRow *row = ReadRow<RepositoryRow>(context, params);
	return row ? static_cast<cell_t>(row->info.fetchTime) : 0;
}

} // namespace

const sp_nativeinfo_t g_Natives[] = {
	{"GitRepo.Open",                GitRepo_Open},
	{"GitRepo.Fetch",               GitRepo_Fetch},
	{"GitRepo.Resolve",             GitRepo_Resolve},
	{"GitRepo.ListRefs",            GitRepo_ListRefs},
	{"GitRepo.Log",                 GitRepo_Log},
	{"GitRepo.Diff",                GitRepo_Diff},
	{"GitRepo.ListTree",            GitRepo_ListTree},
	{"GitRepo.ReadFile",            GitRepo_ReadFile},
	{"GitRepo.WriteFile",           GitRepo_WriteFile},
	{"GitRepo.WriteDir",            GitRepo_WriteDir},
	{"GitRepo.CompareFile",         GitRepo_CompareFile},
	{"GitRepo.Watch",               GitRepo_Watch},
	{"GitRepo.Unwatch",             GitRepo_Unwatch},
	{"GitRepo.Remove",              GitRepo_Remove},
	{"GitRepo.GetUrl",              GitRepo_GetUrl},
	{"GitRepo.GetName",             GitRepo_GetName},
	{"Git_ListRepositories",        Git_ListRepositories},

	{"GitRefList.Length.get",       LengthOf<smgit::Ref>},
	{"GitRefList.GetName",          StringOf<smgit::Ref, &smgit::Ref::name>},
	{"GitRefList.GetSha",           StringOf<smgit::Ref, &smgit::Ref::sha>},
	{"GitRefList.IsTag",            GitRefList_IsTag},
	{"GitRefList.GetHeadBranch",    GitRefList_GetHeadBranch},
	{"GitRefList.GetHeadSha",       GitRefList_GetHeadSha},

	{"GitCommitList.Length.get",    LengthOf<smgit::Commit>},
	{"GitCommitList.GetSha",        StringOf<smgit::Commit, &smgit::Commit::sha>},
	{"GitCommitList.GetAuthorName", StringOf<smgit::Commit, &smgit::Commit::authorName>},
	{"GitCommitList.GetAuthorEmail", StringOf<smgit::Commit, &smgit::Commit::authorEmail>},
	{"GitCommitList.GetTime",       GitCommitList_GetTime},
	{"GitCommitList.GetSummary",    StringOf<smgit::Commit, &smgit::Commit::summary>},
	{"GitCommitList.GetMessage",    StringOf<smgit::Commit, &smgit::Commit::message>},

	{"GitDiff.Length.get",          LengthOf<smgit::DiffEntry>},
	{"GitDiff.GetPath",             StringOf<smgit::DiffEntry, &smgit::DiffEntry::path>},
	{"GitDiff.GetStatus",           GitDiff_GetStatus},
	{"GitDiff.GetOldSha",           StringOf<smgit::DiffEntry, &smgit::DiffEntry::oldSha>},
	{"GitDiff.GetNewSha",           StringOf<smgit::DiffEntry, &smgit::DiffEntry::newSha>},

	{"GitTree.Length.get",          LengthOf<smgit::TreeEntry>},
	{"GitTree.GetPath",             StringOf<smgit::TreeEntry, &smgit::TreeEntry::path>},
	{"GitTree.GetType",             GitTree_GetType},
	{"GitTree.GetSize",             GitTree_GetSize},
	{"GitTree.GetSha",              StringOf<smgit::TreeEntry, &smgit::TreeEntry::sha>},
	{"GitTree.IsExecutable",        GitTree_IsExecutable},

	{"GitChanges.Length.get",       LengthOf<smgit::Change>},
	{"GitChanges.GetPath",          StringOf<smgit::Change, &smgit::Change::path>},
	{"GitChanges.GetKind",          GitChanges_GetKind},

	{"GitRepoList.Length.get",      LengthOf<RepositoryRow>},
	{"GitRepoList.GetName",         RepositoryStringOf<&smgit::RepositoryInfo::name>},
	{"GitRepoList.GetUrl",          RepositoryStringOf<&smgit::RepositoryInfo::url>},
	{"GitRepoList.GetHeadBranch",   RepositoryStringOf<&smgit::RepositoryInfo::headBranch>},
	{"GitRepoList.GetHeadSha",      RepositoryStringOf<&smgit::RepositoryInfo::headSha>},
	{"GitRepoList.GetFetchTime",    GitRepoList_GetFetchTime},
	{"GitRepoList.GetOpeners",      StringOf<RepositoryRow, &RepositoryRow::openers>},
	/* Its name predates the glossary term Watcher: it gives the watched refs. */
	{"GitRepoList.GetWatchers",     StringOf<RepositoryRow, &RepositoryRow::watches>},
	{nullptr,                       nullptr},
};

/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The SourceMod side of the extension. Natives queue git work on one of two
 * workers, the network one for clones, fetches and removals and the local
 * one for everything else, and every game frame the finished work is
 * delivered: to the plugin's callback, unless the plugin unloaded in the
 * meantime. It also keeps who has which repository open, fetches the
 * watched repositories when their schedule (watches.h) says so and tells
 * the plugins about the moves. */
#ifndef SMGIT_EXTENSION_H
#define SMGIT_EXTENSION_H

#include "smsdk_ext.h"

#include "config.h"
#include "repo.h"
#include "watches.h"
#include "worker.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

/* A repository as plugins hold it. */
struct RepoState {
	smgit::Remote remote;
	std::string path;
};
using RepoRef = std::shared_ptr<const RepoState>;

/* What is behind a GitRepo handle. */
struct RepoHandle {
	RepoRef repo;
	std::string ownerName; /* the file of the plugin it was made for, which may be gone by now */
	uint64_t id;           /* never used twice, unlike the handle's address */
};

/* A row of GitRepoList: a repository on disk and who uses it. */
struct RepositoryRow {
	smgit::RepositoryInfo info;
	std::string openers;
	std::string watches;
};

/* What is behind the handles of results; the alternative tells which
 * methodmap it is. */
using ResultData = std::variant<smgit::RefSnapshot, std::vector<smgit::Commit>, std::vector<smgit::DiffEntry>,
                                std::vector<smgit::TreeEntry>, std::vector<smgit::Change>,
                                std::vector<RepositoryRow>>;

using CallbackId = uint64_t;

/* Pushes a callback's arguments between its status and its error text. */
using PayloadPusher = std::function<void(IPluginFunction *, IPluginContext *)>;

class GitExtension : public SDKExtension, public IPluginsListener, public IHandleTypeDispatch {
public: // SDKExtension
	bool SDK_OnLoad(char *error, size_t maxlength, bool late) override;
	void SDK_OnUnload() override;

public: // IPluginsListener
	void OnPluginUnloaded(IPlugin *plugin) override;

public: // IHandleTypeDispatch
	void OnHandleDestroy(HandleType_t type, void *object) override;

public:
	const smgit::Config &Settings() const { return m_config; }
	/* The root and the game directory, both canonical (smgit::CanonicalPath). */
	const std::string &Root() const { return m_root; }
	const std::string &GameDirectory() const { return m_gameDirectory; }
	/* What write-outs must not do on this server. */
	smgit::WritePolicy WriteLimits() const
	{
		return smgit::WritePolicy{m_gameDirectory, m_root, m_config.maxRepositoryBytes};
	}
	/* The one worker for everything that talks to remotes. It must stay the
	 * only thread that does: Mbed TLS is built without its threading layer,
	 * and libgit2 shares one TLS configuration and random generator between
	 * all connections, so two threads with connections would corrupt them.
	 * No network call may run on the local worker or the game thread. */
	smgit::Worker &Network() { return *m_network; }
	smgit::Worker &Local() { return *m_local; }

	/* Remembers a plugin's callback for work about to be queued. False when
	 * function is no function of the plugin. */
	bool Expect(IPluginContext *context, funcid_t function, cell_t data, CallbackId &id);

	/* Calls the callback remembered as id with (status, payload..., error,
	 * data), unless its plugin unloaded. */
	void Deliver(CallbackId id, smgit::Status status, const std::string &error, const PayloadPusher &pushPayload);

	/* The repository of a remote, as every handle on it shares it. */
	RepoRef RepositoryOf(const smgit::Remote &remote) const;

	/* A new GitRepo handle of the plugin. */
	Handle_t CreateRepoHandle(IPluginContext *owner, RepoRef repo);
	/* What is behind a GitRepo handle; null with a native error thrown. */
	RepoHandle *ReadRepoHandle(IPluginContext *context, cell_t handle);

	/* A new result handle of the plugin. */
	Handle_t CreateResultHandle(IPluginContext *owner, ResultData data);
	/* What is behind a result handle; null with a native error thrown. */
	const ResultData *ReadResultHandle(IPluginContext *context, cell_t handle);

	/* The plugins that have the repository open through another handle
	 * than the one with the id exceptHandle, are opening it, or watch it, as
	 * "a.smx, b.smx"; empty when nobody does. */
	std::string UsersOf(const std::string &repoName, uint64_t exceptHandle) const;

	/* A GitRepo.Open of the plugin pluginName, answered as id, from when it
	 * queued work on the network worker until it is answered: the plugin
	 * counts as a user of the repository. */
	void BeginOpen(CallbackId id, const std::string &repoName, const std::string &pluginName);
	void EndOpen(CallbackId id);
	/* The file of the plugin, for telling who uses a repository. */
	std::string NameOfPlugin(IPluginContext *context) const;

	/* A removal that passed the check for users, from then until it ended.
	 * Meanwhile GitRepo.Open of the repository waits for it. */
	void BeginRemoval(const std::string &repoName);
	void EndRemoval(const std::string &repoName);
	bool IsBeingRemoved(const std::string &repoName) const;
	/* How many removals of the repository passed their check so far: an
	 * open whose look at the disk is older than the last one saw what may
	 * be gone. */
	uint64_t RemovalsStartedOf(const std::string &repoName) const;
	/* The plugins that have the repository open, as "a.smx, b.smx". */
	std::string OpenersOf(const std::string &repoName) const;
	/* The repository's watched refs, as "main (300 s), v1 (60 s)". */
	std::string WatchesOf(const std::string &repoName) const;

	/* As smgit::WatchSchedule::Watch, with the plugin as the watcher. */
	void Watch(IPluginContext *plugin, RepoRef repo, const std::string &ref, int interval);
	void Unwatch(IPluginContext *plugin, const std::string &repoName, const std::string &ref);

	/* Tells the plugins about every watched ref of the repository that is
	 * somewhere else than when it was last seen. */
	void ObserveRefs(const std::string &repoName, const smgit::RefSnapshot &refs);

	/* Runs the completions of finished work and polls the watches due. */
	void OnFrame();

private:
	struct PendingCallback {
		IPluginContext *context;
		funcid_t function;
		cell_t data;
	};

	using Clock = smgit::WatchClock;

	bool CreateApi(std::string &error);
	void ReleaseApi();
	bool LoadSettings(std::string &error);
	bool ResolveDirectories(std::string &error);
	void SweepLeftovers();
	void WriteNotedLog();
	bool PrepareCaBundle(std::string &bundle, std::string &error);
	void PollDueWatches();
	void OnPolled(const std::string &repoName, const smgit::Result<smgit::FetchReport> &fetched);
	void Announce(const std::vector<smgit::RefMove> &moves);

	smgit::Config m_config;
	std::string m_root;
	std::string m_gameDirectory;
	bool m_isGitStarted = false;
	std::unique_ptr<smgit::Worker> m_network;
	std::unique_ptr<smgit::Worker> m_local;

	HandleType_t m_repoType = 0;
	HandleType_t m_resultType = 0;
	IForward *m_refChanged = nullptr;

	std::unordered_map<CallbackId, PendingCallback> m_pending;
	CallbackId m_nextCallbackId = 1;
	uint64_t m_nextHandleId = 1;
	std::map<std::string, std::vector<RepoHandle *>> m_openHandles; /* by repository name */
	struct PendingOpen {
		std::string repoName;
		std::string pluginName;
	};
	std::map<CallbackId, PendingOpen> m_pendingOpens;
	std::map<std::string, int> m_removals;                          /* running removals by repository name */
	std::map<std::string, uint64_t> m_removalsStarted;              /* every removal by repository name */
	std::unique_ptr<smgit::WatchSchedule> m_watches;
	Clock::time_point m_lastPollCheck;
};

extern GitExtension g_Git;
extern const sp_nativeinfo_t g_Natives[];

#endif // SMGIT_EXTENSION_H

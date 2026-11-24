/* Copyright 2026 ETF2L Contributors, Dave */
#include "extension.h"

#include "serverlog.h"

#include <stdio.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <utility>

using smgit::Status;

GitExtension g_Git;
SMEXT_LINK(&g_Git);

namespace {

const char kSettingsFile[] = "configs/git.cfg";
const char kShippedCaBundle[] = "configs/git/ca-bundle.crt";
const char kCombinedCaBundle[] = "data/git-ca-bundle.crt";
const char kDefaultRoot[] = "data/git";

void OnGameFrame(bool simulating)
{
	g_Git.OnFrame();
}

std::string SourceModPath(const char *relative)
{
	char buffer[PLATFORM_MAX_PATH];
	smutils->BuildPath(Path_SM, buffer, sizeof(buffer), "%s", relative);
	return buffer;
}

/* A path of the settings: absolute as it is, otherwise below the game
 * directory. */
std::string SettingsPath(const std::string &path)
{
	if (!path.empty() && path.front() == '/')
		return path;
	char buffer[PLATFORM_MAX_PATH];
	smutils->BuildPath(Path_Game, buffer, sizeof(buffer), "%s", path.c_str());
	return buffer;
}

/* Collects the keys and values of the settings file's one section. */
class SettingsReader : public ITextListener_SMC {
public:
	SMCResult ReadSMC_NewSection(const SMCStates *states, const char *name) override
	{
		return ++m_depth == 1 ? SMCResult_Continue : SMCResult_HaltFail;
	}

	SMCResult ReadSMC_KeyValue(const SMCStates *states, const char *key, const char *value) override
	{
		values[key] = value;
		return SMCResult_Continue;
	}

	SMCResult ReadSMC_LeavingSection(const SMCStates *states) override
	{
		m_depth--;
		return SMCResult_Continue;
	}

	std::map<std::string, std::string> values;

private:
	int m_depth = 0;
};

bool FileExists(const std::string &file)
{
	return std::ifstream(file).good();
}

/* Writes the content of both files, one after the other, to combined. */
bool Concatenate(const std::string &first, const std::string &second, const std::string &combined)
{
	std::ifstream firstIn(first, std::ios::binary);
	std::ifstream secondIn(second, std::ios::binary);
	if (!firstIn || !secondIn)
		return false;
	std::ofstream out(combined, std::ios::binary | std::ios::trunc);
	out << firstIn.rdbuf() << '\n' << secondIn.rdbuf();
	out.close();
	return out.good();
}

std::string Join(const std::set<std::string> &parts)
{
	std::string joined;
	for (const std::string &part : parts)
		joined += (joined.empty() ? "" : ", ") + part;
	return joined;
}

} // namespace

bool GitExtension::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	std::string reason;
	std::string caBundle;
	if (!LoadSettings(reason) || !ResolveDirectories(reason) || !PrepareCaBundle(caBundle, reason) ||
	    !CreateApi(reason)) {
		snprintf(error, maxlength, "%s", reason.c_str());
		return false;
	}
	reason = smgit::Initialize(m_config, caBundle);
	if (!reason.empty()) {
		ReleaseApi();
		snprintf(error, maxlength, "%s", reason.c_str());
		return false;
	}
	m_isGitStarted = true;

	m_network = std::make_unique<smgit::Worker>();
	m_local = std::make_unique<smgit::Worker>();
	m_watches = std::make_unique<smgit::WatchSchedule>(m_config.watchInterval);
	SweepLeftovers();

	sharesys->AddNatives(myself, g_Natives);
	sharesys->RegisterLibrary(myself, "git");
	plsys->AddPluginsListener(this);
	smutils->AddGameFrameHook(OnGameFrame);
	return true;
}

void GitExtension::SDK_OnUnload()
{
	if (!m_isGitStarted)
		return;
	smutils->RemoveGameFrameHook(OnGameFrame);
	plsys->RemovePluginsListener(this);
	m_network.reset();
	m_local.reset();
	m_pending.clear();
	m_pendingOpens.clear();
	m_removals.clear();
	m_removalsStarted.clear();
	m_watches.reset();
	ReleaseApi();
	smgit::Shutdown();
	m_isGitStarted = false;
}

/* The handle types and the forward of the plugin API. SourceMod does not
 * unload an extension whose SDK_OnLoad failed, so a failure here releases
 * what was made already. */
bool GitExtension::CreateApi(std::string &error)
{
	m_repoType = handlesys->CreateType("GitRepo", this, 0, nullptr, nullptr, myself->GetIdentity(), nullptr);
	m_resultType = handlesys->CreateType("GitResult", this, 0, nullptr, nullptr, myself->GetIdentity(), nullptr);
	m_refChanged = forwards->CreateForward("Git_OnRefChanged", ET_Ignore, 4, nullptr, Param_String, Param_String,
	                                       Param_String, Param_String);
	if (m_repoType != 0 && m_resultType != 0 && m_refChanged)
		return true;

	error = m_repoType == 0     ? "cannot create the handle type GitRepo; another extension may have taken the name"
	        : m_resultType == 0 ? "cannot create the handle type GitResult; another extension may have taken the name"
	                            : "cannot create the forward Git_OnRefChanged";
	ReleaseApi();
	return false;
}

void GitExtension::ReleaseApi()
{
	if (m_repoType != 0)
		handlesys->RemoveType(m_repoType, myself->GetIdentity());
	if (m_resultType != 0)
		handlesys->RemoveType(m_resultType, myself->GetIdentity());
	if (m_refChanged)
		forwards->ReleaseForward(m_refChanged);
	m_repoType = 0;
	m_resultType = 0;
	m_refChanged = nullptr;
}

bool GitExtension::LoadSettings(std::string &error)
{
	std::string file = SourceModPath(kSettingsFile);
	SettingsReader reader;
	if (FileExists(file)) {
		SMCStates states;
		SMCError parseError = textparsers->ParseFile_SMC(file.c_str(), &reader, &states);
		if (parseError != SMCError_Okay) {
			error = file + ", line " + std::to_string(states.line) + ": " +
			        textparsers->GetSMCErrorString(parseError);
			return false;
		}
	}

	std::optional<smgit::Config> config = smgit::ParseConfig(reader.values, error);
	if (!config) {
		error = file + ": " + error;
		return false;
	}
	m_config = *config;
	return true;
}

/* The root and the game directory as write-outs compare destinations with
 * them: resolved once, symlinks and all. */
bool GitExtension::ResolveDirectories(std::string &error)
{
	std::string root = m_config.root.empty() ? SourceModPath(kDefaultRoot) : SettingsPath(m_config.root);
	smgit::Result<std::string> canonicalRoot = smgit::CanonicalPath(root);
	smgit::Result<std::string> canonicalGame = smgit::CanonicalPath(smutils->GetGamePath());
	if (!canonicalRoot.ok() || !canonicalGame.ok()) {
		error = canonicalRoot.ok() ? canonicalGame.error : canonicalRoot.error;
		return false;
	}
	m_root = canonicalRoot.value;
	m_gameDirectory = canonicalGame.value;
	return true;
}

/* Deletes what a crash of this or another server sharing the root left
 * there, on the local worker, ahead of any work of the plugins. */
void GitExtension::SweepLeftovers()
{
	std::string root = m_root;
	m_local->Submit([root](const std::atomic<bool> &) -> smgit::Completion {
		smgit::SweepReport report = smgit::SweepLeftovers(root);
		return [report] {
			for (const std::string &removed : report.removed)
				smutils->LogMessage(myself, "Deleted the leftover %s", removed.c_str());
			for (const std::string &problem : report.problems)
				smutils->LogError(myself, "Cleaning up the root: %s", problem.c_str());
		};
	});
}

/* What the workers noted for the log. */
void GitExtension::WriteNotedLog()
{
	for (const smgit::LogLine &line : smgit::TakeLogLines()) {
		if (line.level == smgit::LogLevel::Error)
			smutils->LogError(myself, "%s", line.text.c_str());
		else
			smutils->LogMessage(myself, "%s", line.text.c_str());
	}
}

/* libgit2 takes one file of CA certificates: the shipped bundle, or with a
 * ca_file a file made of both. */
bool GitExtension::PrepareCaBundle(std::string &bundle, std::string &error)
{
	std::string shipped = SourceModPath(kShippedCaBundle);
	if (m_config.caFile.empty()) {
		bundle = shipped;
		return true;
	}

	std::string extra = SettingsPath(m_config.caFile);
	bundle = SourceModPath(kCombinedCaBundle);
	if (Concatenate(shipped, extra, bundle))
		return true;
	error = "cannot combine " + shipped + " and the ca_file " + extra + " into " + bundle;
	return false;
}

void GitExtension::OnPluginUnloaded(IPlugin *plugin)
{
	IPluginContext *context = plugin->GetBaseContext();
	for (auto it = m_pending.begin(); it != m_pending.end();) {
		if (it->second.context == context)
			it = m_pending.erase(it);
		else
			++it;
	}
	m_watches->Forget(context);
}

void GitExtension::OnHandleDestroy(HandleType_t type, void *object)
{
	if (type == m_resultType) {
		delete static_cast<ResultData *>(object);
		return;
	}

	RepoHandle *handle = static_cast<RepoHandle *>(object);
	auto found = m_openHandles.find(handle->repo->remote.name);
	if (found != m_openHandles.end()) {
		std::vector<RepoHandle *> &handles = found->second;
		handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
		if (handles.empty())
			m_openHandles.erase(found);
	}
	delete handle;
}

bool GitExtension::Expect(IPluginContext *context, funcid_t function, cell_t data, CallbackId &id)
{
	IPluginFunction *found;
	if (!context->GetFunctionByIdOrNull(function, &found) || !found)
		return false;
	id = m_nextCallbackId++;
	m_pending.emplace(id, PendingCallback{context, function, data});
	return true;
}

void GitExtension::Deliver(CallbackId id, Status status, const std::string &error, const PayloadPusher &pushPayload)
{
	auto found = m_pending.find(id);
	if (found == m_pending.end())
		return;
	PendingCallback callback = found->second;
	m_pending.erase(found);

	IPluginFunction *function;
	if (!callback.context->GetFunctionByIdOrNull(callback.function, &function) || !function)
		return;
	function->PushCell(static_cast<cell_t>(status));
	pushPayload(function, callback.context);
	function->PushString(error.c_str());
	function->PushCell(callback.data);
	function->Execute(nullptr);
}

RepoRef GitExtension::RepositoryOf(const smgit::Remote &remote) const
{
	return std::make_shared<const RepoState>(RepoState{remote, smgit::RepositoryPath(m_root, remote)});
}

Handle_t GitExtension::CreateRepoHandle(IPluginContext *owner, RepoRef repo)
{
	RepoHandle *handle = new RepoHandle{std::move(repo), NameOfPlugin(owner), m_nextHandleId++};
	Handle_t created = handlesys->CreateHandle(m_repoType, handle, owner->GetIdentity(), myself->GetIdentity(),
	                                           nullptr);
	if (created == BAD_HANDLE) {
		delete handle;
		return BAD_HANDLE;
	}
	m_openHandles[handle->repo->remote.name].push_back(handle);
	return created;
}

RepoHandle *GitExtension::ReadRepoHandle(IPluginContext *context, cell_t handle)
{
	HandleSecurity security(nullptr, myself->GetIdentity());
	RepoHandle *repo;
	HandleError failure = handlesys->ReadHandle(static_cast<Handle_t>(handle), m_repoType, &security,
	                                            reinterpret_cast<void **>(&repo));
	if (failure != HandleError_None) {
		context->ReportError("Invalid GitRepo handle %x (error %d)", handle, failure);
		return nullptr;
	}
	return repo;
}

Handle_t GitExtension::CreateResultHandle(IPluginContext *owner, ResultData data)
{
	ResultData *result = new ResultData(std::move(data));
	Handle_t created = handlesys->CreateHandle(m_resultType, result, owner->GetIdentity(), myself->GetIdentity(),
	                                           nullptr);
	if (created == BAD_HANDLE)
		delete result;
	return created;
}

const ResultData *GitExtension::ReadResultHandle(IPluginContext *context, cell_t handle)
{
	HandleSecurity security(nullptr, myself->GetIdentity());
	ResultData *result;
	HandleError failure = handlesys->ReadHandle(static_cast<Handle_t>(handle), m_resultType, &security,
	                                            reinterpret_cast<void **>(&result));
	if (failure != HandleError_None) {
		context->ReportError("Invalid git result handle %x (error %d)", handle, failure);
		return nullptr;
	}
	return result;
}

std::string GitExtension::NameOfPlugin(IPluginContext *context) const
{
	IPlugin *plugin = plsys->FindPluginByContext(context->GetContext());
	return plugin ? plugin->GetFilename() : "?";
}

std::string GitExtension::OpenersOf(const std::string &repoName) const
{
	std::set<std::string> plugins;
	auto found = m_openHandles.find(repoName);
	if (found == m_openHandles.end())
		return {};
	for (const RepoHandle *handle : found->second)
		plugins.insert(handle->ownerName);
	return Join(plugins);
}

std::string GitExtension::WatchesOf(const std::string &repoName) const
{
	return m_watches->Describe(repoName);
}

std::string GitExtension::UsersOf(const std::string &repoName, uint64_t exceptHandle) const
{
	std::set<std::string> plugins;
	auto open = m_openHandles.find(repoName);
	if (open != m_openHandles.end()) {
		for (const RepoHandle *handle : open->second) {
			if (handle->id != exceptHandle)
				plugins.insert(handle->ownerName);
		}
	}
	for (const auto &[id, opening] : m_pendingOpens) {
		if (opening.repoName == repoName)
			plugins.insert(opening.pluginName);
	}
	for (smgit::Watcher watcher : m_watches->WatchersOf(repoName))
		plugins.insert(NameOfPlugin(static_cast<IPluginContext *>(const_cast<void *>(watcher))));
	return Join(plugins);
}

void GitExtension::BeginOpen(CallbackId id, const std::string &repoName, const std::string &pluginName)
{
	m_pendingOpens[id] = PendingOpen{repoName, pluginName};
}

void GitExtension::EndOpen(CallbackId id)
{
	m_pendingOpens.erase(id);
}

void GitExtension::BeginRemoval(const std::string &repoName)
{
	m_removals[repoName]++;
	m_removalsStarted[repoName]++;
}

void GitExtension::EndRemoval(const std::string &repoName)
{
	auto found = m_removals.find(repoName);
	if (found != m_removals.end() && --found->second == 0)
		m_removals.erase(found);
}

bool GitExtension::IsBeingRemoved(const std::string &repoName) const
{
	return m_removals.count(repoName) != 0;
}

uint64_t GitExtension::RemovalsStartedOf(const std::string &repoName) const
{
	auto found = m_removalsStarted.find(repoName);
	return found == m_removalsStarted.end() ? 0 : found->second;
}

void GitExtension::Watch(IPluginContext *plugin, RepoRef repo, const std::string &ref, int interval)
{
	m_watches->Watch(repo->remote, repo->path, ref, plugin, interval, Clock::now());
}

void GitExtension::Unwatch(IPluginContext *plugin, const std::string &repoName, const std::string &ref)
{
	m_watches->Unwatch(repoName, ref, plugin);
}

void GitExtension::ObserveRefs(const std::string &repoName, const smgit::RefSnapshot &refs)
{
	Announce(m_watches->Observe(repoName, refs));
}

/* Collected before the first is told: the forward may run plugin code that
 * changes the watches. */
void GitExtension::Announce(const std::vector<smgit::RefMove> &moves)
{
	for (const smgit::RefMove &move : moves) {
		m_refChanged->PushString(move.url.c_str());
		m_refChanged->PushString(move.ref.c_str());
		m_refChanged->PushString(move.oldSha.c_str());
		m_refChanged->PushString(move.newSha.c_str());
		m_refChanged->Execute(nullptr);
	}
}

void GitExtension::PollDueWatches()
{
	Clock::time_point now = Clock::now();
	if (now - m_lastPollCheck < std::chrono::seconds(1))
		return;
	m_lastPollCheck = now;

	uint64_t maxBytes = m_config.maxRepositoryBytes;
	for (const smgit::DuePoll &due : m_watches->TakeDue(now)) {
		std::string name = due.repoName;
		std::string path = due.path;
		m_network->Submit([name, path, maxBytes](const std::atomic<bool> &abort) -> smgit::Completion {
			smgit::Result<smgit::FetchReport> fetched =
				smgit::Guarded([&] { return smgit::Fetch(path, maxBytes, abort); });
			return [name, fetched] { g_Git.OnPolled(name, fetched); };
		});
	}
}

void GitExtension::OnPolled(const std::string &repoName, const smgit::Result<smgit::FetchReport> &fetched)
{
	if (!m_watches->IsWatched(repoName))
		return;
	if (!fetched.ok())
		smutils->LogError(myself, "Polling %s failed: %s", repoName.c_str(), fetched.error.c_str());
	Announce(m_watches->FinishPoll(repoName, fetched, Clock::now()));
}

void GitExtension::OnFrame()
{
	/* An exception must not leave the game frame, which would end the
	 * server; the callback of a completion that threw is lost. */
	for (smgit::Worker *worker : {m_network.get(), m_local.get()}) {
		for (const smgit::Completion &completion : worker->TakeCompletions()) {
			try {
				completion();
			} catch (const std::exception &failure) {
				smutils->LogError(myself, "Delivering a result failed: %s", failure.what());
			} catch (...) {
				smutils->LogError(myself, "Delivering a result failed with an unknown exception");
			}
		}
	}
	WriteNotedLog();
	PollDueWatches();
}

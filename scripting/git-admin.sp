/**
 * Copyright 2026 ETF2L Contributors, Dave
 *
 * git-admin: the admin command of the git extension.
 *
 *   sm_git list                       the repositories below the root
 *   sm_git status <repo>              one repository with its branches and tags
 *   sm_git log <repo> [rev] [count]   its newest commits
 *   sm_git fetch <repo>               fetch it now
 *   sm_git clone "<url>"              clone a repository
 *   sm_git remove <name|url>          delete it from disk
 *
 * <repo> is a repository's name ("github.com/owner/name") or URL, or any
 * part of the name that only one repository has; remove takes only the
 * exact name or URL. URLs and names with a port must be in quotes: the
 * console splits words at ":" and takes "//" as the start of a comment.
 * The git work
 * happens in the background, so the answer comes a moment after the
 * command, to the chat or console it was given in; a command given over
 * RCON is answered in the server console.
 */

#include <sourcemod>
#include <git>

#pragma semicolon 1
#pragma newdecls required

#define SHORT_SHA_LENGTH 7
#define DEFAULT_LOG_COUNT 10
#define MAX_LOG_COUNT 50

public Plugin myinfo = {
	name = "git-admin",
	author = "ETF2L Contributors, Dave",
	description = "Admin command of the git extension",
	version = "1.0.0",
	url = "https://github.com/etf2l/sm-git"
};

/**
 * A command on its way through the callbacks: a StringMap with who asked
 * ("userid", "source"), what ("action", "rev", "count") and, once found,
 * the repository's row of the list ("name", "url", "fetched", "openers",
 * "watchers") and its handle ("repo").
 */
methodmap Request < StringMap {
	public Request(int client, const char[] action) {
		StringMap request = new StringMap();
		request.SetValue("userid", client == 0 ? 0 : GetClientUserId(client));
		request.SetValue("source", GetCmdReplySource());
		request.SetString("action", action);
		return view_as<Request>(request);
	}

	property GitRepo Repo {
		public get() {
			GitRepo repo = null;
			this.GetValue("repo", repo);
			return repo;
		}
		public set(GitRepo repo) {
			this.SetValue("repo", repo);
		}
	}

	/** Answers one line to whoever gave the command, if they are still there. */
	public void Tell(const char[] format, any ...) {
		int userid;
		this.GetValue("userid", userid);
		ReplySource source;
		this.GetValue("source", source);
		int client = userid == 0 ? 0 : GetClientOfUserId(userid);
		if (userid != 0 && client == 0) {
			return;
		}

		char line[512];
		VFormat(line, sizeof(line), format, 3);
		ReplySource previous = SetCmdReplySource(source);
		ReplyToCommand(client, "%s", line);
		SetCmdReplySource(previous);
	}

	/** Answers "<name>: ..." */
	public void TellAbout(const char[] format, any ...) {
		char name[PLATFORM_MAX_PATH];
		this.GetString("name", name, sizeof(name));
		char text[384];
		VFormat(text, sizeof(text), format, 3);
		this.Tell("%s: %s", name, text);
	}

	/** Ends the command: closes its repository and forgets it. */
	public void Finish() {
		GitRepo repo = this.Repo;
		delete repo;
		delete this;
	}
}

public void OnPluginStart() {
	RegAdminCmd("sm_git", Command_Git, ADMFLAG_ROOT,
	            "sm_git <list|status|log|fetch|clone|remove> ...; sm_git clone \"<url>\"");
}

Action Command_Git(int client, int args) {
	char action[16];
	GetCmdArg(1, action, sizeof(action));
	char target[PLATFORM_MAX_PATH];
	GetCmdArg(2, target, sizeof(target));

	if (StrEqual(action, "list") && args == 1) {
		Git_ListRepositories(OnListed, new Request(client, action));
		return Plugin_Handled;
	}
	// The console made "https", ":" and a comment of a URL without quotes.
	if (args > 2 && StrEqual(target, "https")) {
		ReplyToCommand(client, "sm_git %s: put the URL in quotes, sm_git %s \"https://...\"", action, action);
		return Plugin_Handled;
	}
	if (StrEqual(action, "clone") && args == 2) {
		Request request = new Request(client, action);
		request.SetString("name", target);
		GitRepo.Open(target, OnOpened, request);
		return Plugin_Handled;
	}

	bool isLog = StrEqual(action, "log");
	bool isNamed = isLog || StrEqual(action, "status") || StrEqual(action, "fetch") || StrEqual(action, "remove");
	if (!isNamed || args < 2 || args > (isLog ? 4 : 2)) {
		ReplyUsage(client);
		return Plugin_Handled;
	}

	int count = DEFAULT_LOG_COUNT;
	char rev[128] = "HEAD";
	if (args >= 3) {
		GetCmdArg(3, rev, sizeof(rev));
	}
	if (args >= 4) {
		count = GetCmdArgInt(4);
	}
	if (rev[0] == '\0' || count < 1 || count > MAX_LOG_COUNT) {
		ReplyToCommand(client, "sm_git log: rev must not be empty and count must be 1 to %d", MAX_LOG_COUNT);
		return Plugin_Handled;
	}

	Request request = new Request(client, action);
	request.SetString("wanted", target);
	request.SetString("rev", rev);
	request.SetValue("count", count);
	Git_ListRepositories(OnListedToFind, request);
	return Plugin_Handled;
}

void ReplyUsage(int client) {
	ReplyToCommand(client, "Usage: sm_git list");
	ReplyToCommand(client, "       sm_git status <repo>");
	ReplyToCommand(client, "       sm_git log <repo> [rev] [count]");
	ReplyToCommand(client, "       sm_git fetch <repo>");
	ReplyToCommand(client, "       sm_git clone \"<url>\"");
	ReplyToCommand(client, "       sm_git remove <name|url>");
}

/** "4 min ago", "2 h ago", "1 day ago"; "never" for no time. */
void FormatAge(int time, char[] buffer, int maxlength) {
	if (time == 0) {
		strcopy(buffer, maxlength, "never");
		return;
	}
	int seconds = GetTime() - time;
	if (seconds < 60) {
		Format(buffer, maxlength, "%d s ago", seconds < 0 ? 0 : seconds);
	} else if (seconds < 3600) {
		Format(buffer, maxlength, "%d min ago", seconds / 60);
	} else if (seconds < 86400) {
		Format(buffer, maxlength, "%d h ago", seconds / 3600);
	} else {
		Format(buffer, maxlength, "%d day%s ago", seconds / 86400, seconds / 86400 == 1 ? "" : "s");
	}
}

/** "-" for nothing. */
void DashIfEmpty(char[] text, int maxlength) {
	if (text[0] == '\0') {
		strcopy(text, maxlength, "-");
	}
}

/** "main 3f9c2ab", or "(empty)" for a repository without commits. */
void FormatHead(const char[] branch, const char[] sha, char[] buffer, int maxlength) {
	if (sha[0] == '\0') {
		strcopy(buffer, maxlength, "(empty)");
		return;
	}
	char shortSha[SHORT_SHA_LENGTH + 1];
	strcopy(shortSha, sizeof(shortSha), sha);
	Format(buffer, maxlength, "%s %s", branch, shortSha);
}

// ---------------------------------------------------------------------- list

void OnListed(GitStatus status, GitRepoList repos, const char[] error, Request request) {
	if (status != Git_OK) {
		request.Tell("sm_git list: %s", error);
		request.Finish();
		return;
	}
	if (repos.Length == 0) {
		request.Tell("No repositories.");
	}

	int nameWidth = 0;
	char name[PLATFORM_MAX_PATH];
	for (int i = 0; i < repos.Length; i++) {
		int length = repos.GetName(i, name, sizeof(name));
		if (length > nameWidth) {
			nameWidth = length;
		}
	}
	char lineFormat[32];
	Format(lineFormat, sizeof(lineFormat), "%%-%ds   %%s   fetched %%s", nameWidth);

	for (int i = 0; i < repos.Length; i++) {
		repos.GetName(i, name, sizeof(name));
		char branch[128];
		repos.GetHeadBranch(i, branch, sizeof(branch));
		char sha[64];
		repos.GetHeadSha(i, sha, sizeof(sha));
		char openers[256];
		repos.GetOpeners(i, openers, sizeof(openers));
		char watchers[256];
		repos.GetWatchers(i, watchers, sizeof(watchers));
		char head[192];
		FormatHead(branch, sha, head, sizeof(head));
		char age[32];
		FormatAge(repos.GetFetchTime(i), age, sizeof(age));
		DashIfEmpty(openers, sizeof(openers));
		DashIfEmpty(watchers, sizeof(watchers));

		request.Tell(lineFormat, name, head, age);
		request.Tell("    open: %s   watching: %s", openers, watchers);
	}
	delete repos;
	request.Finish();
}

// ------------------------------------------------- finding the named repository

/**
 * The row whose name or URL is wanted; unless only an exact match counts,
 * also the only row whose name contains it. -1 with an answer otherwise.
 */
int FindRepository(Request request, GitRepoList repos, const char[] wanted, bool isExactOnly) {
	int found = -1;
	int matches = 0;
	char candidates[384];
	for (int i = 0; i < repos.Length; i++) {
		char name[PLATFORM_MAX_PATH];
		repos.GetName(i, name, sizeof(name));
		char url[PLATFORM_MAX_PATH];
		repos.GetUrl(i, url, sizeof(url));
		if (StrEqual(name, wanted) || StrEqual(url, wanted)) {
			return i;
		}
		if (isExactOnly || StrContains(name, wanted) == -1) {
			continue;
		}
		found = i;
		matches++;
		Format(candidates, sizeof(candidates), "%s%s%s", candidates, candidates[0] == '\0' ? "" : ", ", name);
	}
	if (matches == 1) {
		return found;
	}
	if (isExactOnly) {
		request.Tell("No repository is named \"%s\"; remove takes the exact name or URL.", wanted);
	} else if (matches == 0) {
		request.Tell("No repository matches \"%s\".", wanted);
	} else {
		request.Tell("Several repositories match \"%s\": %s", wanted, candidates);
	}
	return -1;
}

void OnListedToFind(GitStatus status, GitRepoList repos, const char[] error, Request request) {
	if (status != Git_OK) {
		request.Tell("sm_git: %s", error);
		request.Finish();
		return;
	}

	char wanted[PLATFORM_MAX_PATH];
	request.GetString("wanted", wanted, sizeof(wanted));
	char action[16];
	request.GetString("action", action, sizeof(action));
	// Removal cannot be undone, for every server sharing the root: no guessing.
	int row = FindRepository(request, repos, wanted, StrEqual(action, "remove"));
	if (row == -1) {
		delete repos;
		request.Finish();
		return;
	}

	char name[PLATFORM_MAX_PATH];
	repos.GetName(row, name, sizeof(name));
	char url[PLATFORM_MAX_PATH];
	repos.GetUrl(row, url, sizeof(url));
	char openers[256];
	repos.GetOpeners(row, openers, sizeof(openers));
	char watchers[256];
	repos.GetWatchers(row, watchers, sizeof(watchers));
	char age[32];
	FormatAge(repos.GetFetchTime(row), age, sizeof(age));
	DashIfEmpty(openers, sizeof(openers));
	DashIfEmpty(watchers, sizeof(watchers));
	delete repos;

	request.SetString("name", name);
	request.SetString("url", url);
	request.SetString("fetched", age);
	request.SetString("openers", openers);
	request.SetString("watchers", watchers);
	// Another server sharing the root may have removed it since the list: looking must not clone it again.
	GitRepo.Open(url, OnOpened, request, false);
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, Request request) {
	if (status != Git_OK) {
		request.TellAbout("%s", error);
		request.Finish();
		return;
	}
	request.Repo = repo;

	char action[16];
	request.GetString("action", action, sizeof(action));
	if (StrEqual(action, "clone")) {
		char name[PLATFORM_MAX_PATH];
		repo.GetName(name, sizeof(name));
		request.SetString("name", name);
		request.SetValue("cloned", cloned);
		repo.ListRefs(OnRefsListed, request);
	} else if (StrEqual(action, "status")) {
		repo.ListRefs(OnRefsListed, request);
	} else if (StrEqual(action, "log")) {
		char rev[128];
		request.GetString("rev", rev, sizeof(rev));
		int count;
		request.GetValue("count", count);
		repo.Log(rev, OnLogged, request, "", count);
	} else if (StrEqual(action, "fetch")) {
		repo.Fetch(OnFetched, request);
	} else {
		repo.Remove(OnRemoved, request);
	}
}

// ------------------------------------------------------------ status and clone

/** "main 3f9c2ab, dev 81d07e4" of the branches or of the tags; "-" for none. */
void JoinRefs(GitRefList refs, bool tags, char[] buffer, int maxlength) {
	buffer[0] = '\0';
	for (int i = 0; i < refs.Length; i++) {
		if (refs.IsTag(i) != tags) {
			continue;
		}
		char name[128];
		refs.GetName(i, name, sizeof(name));
		char sha[64];
		refs.GetSha(i, sha, sizeof(sha));
		char entry[192];
		FormatHead(name, sha, entry, sizeof(entry));
		Format(buffer, maxlength, "%s%s%s", buffer, buffer[0] == '\0' ? "" : ", ", entry);
	}
	DashIfEmpty(buffer, maxlength);
}

void OnRefsListed(GitStatus status, GitRefList refs, const char[] error, Request request) {
	if (status != Git_OK) {
		request.TellAbout("%s", error);
		request.Finish();
		return;
	}

	char branch[128];
	refs.GetHeadBranch(branch, sizeof(branch));
	char sha[64];
	refs.GetHeadSha(sha, sizeof(sha));
	char head[192];
	FormatHead(branch, sha, head, sizeof(head));

	bool cloned;
	if (request.GetValue("cloned", cloned)) {
		request.TellAbout("%s (%s)", cloned ? "cloned" : "already there", head);
		delete refs;
		request.Finish();
		return;
	}

	char name[PLATFORM_MAX_PATH];
	request.GetString("name", name, sizeof(name));
	char url[PLATFORM_MAX_PATH];
	request.GetString("url", url, sizeof(url));
	char age[32];
	request.GetString("fetched", age, sizeof(age));
	char openers[256];
	request.GetString("openers", openers, sizeof(openers));
	char watchers[256];
	request.GetString("watchers", watchers, sizeof(watchers));
	char branches[400];
	JoinRefs(refs, false, branches, sizeof(branches));
	char tags[400];
	JoinRefs(refs, true, tags, sizeof(tags));
	delete refs;

	request.Tell("%s", name);
	request.Tell("  url       %s", url);
	request.Tell("  head      %s", head);
	request.Tell("  fetched   %s", age);
	request.Tell("  open      %s", openers);
	request.Tell("  watching  %s", watchers);
	request.Tell("  branches  %s", branches);
	request.Tell("  tags      %s", tags);
	request.Finish();
}

// ------------------------------------------------------------------------ log

void OnLogged(GitStatus status, GitCommitList commits, const char[] error, Request request) {
	if (status != Git_OK) {
		request.TellAbout("%s", error);
		request.Finish();
		return;
	}

	for (int i = 0; i < commits.Length; i++) {
		char sha[64];
		commits.GetSha(i, sha, sizeof(sha));
		char author[128];
		commits.GetAuthorName(i, author, sizeof(author));
		char summary[256];
		commits.GetSummary(i, summary, sizeof(summary));
		char age[32];
		FormatAge(commits.GetTime(i), age, sizeof(age));
		sha[SHORT_SHA_LENGTH] = '\0';
		request.Tell("%s %s, %s: %s", sha, age, author, summary);
	}
	delete commits;
	request.Finish();
}

// ----------------------------------------------------------- fetch and remove

void OnFetched(GitStatus status, bool changed, const char[] error, Request request) {
	if (status != Git_OK) {
		request.TellAbout("%s", error);
	} else {
		request.TellAbout("fetched, %s", changed ? "refs changed" : "nothing new");
	}
	request.Finish();
}

void OnRemoved(GitStatus status, const char[] error, Request request) {
	if (status != Git_OK) {
		request.TellAbout("%s", error);
	} else {
		request.TellAbout("removed");
	}
	request.Finish();
}

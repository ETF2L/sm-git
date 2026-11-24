# sm-git examples

Complete plugins, one task each. Each was compiled and run against a real HTTPS git server (the project's test
server); the outputs shown are what they printed. The [reference](reference.md) explains each native in full.

- [Follow a branch](#follow-a-branch): watch `main`, write its `cfg/` into the server's `cfg/`
- [Read one file](#read-one-file): `ReadFile`, with room on the heap
- [Show recent changes](#show-recent-changes): `Log` of one directory
- [Install the newest release tag](#install-the-newest-release-tag): `Fetch`, `ListRefs`, `Diff`, `WriteFile`
- [Check the server's files](#check-the-servers-files): `ListTree`, `CompareFile`, opening without cloning
- [List the repositories](#list-the-repositories): `Git_ListRepositories`
- [Remove a repository](#remove-a-repository): `Unwatch`, `Remove`

All examples use made-up `https://github.com/owner/...` URLs. Replace them with real ones. The outputs show the test
server's URL; plugin file names are shortened.

## Follow a branch

Keeps the server's `cfg/` in step with the `cfg/` of a branch. Files the branch drops are deleted on the server.

```sourcepawn
#include <sourcemod>
#include <git>

#define CONFIGS_URL    "https://github.com/owner/configs"
#define CONFIGS_BRANCH "main"

GitRepo g_Repo;

public void OnPluginStart()
{
	GitRepo.Open(CONFIGS_URL, OnOpened);       // clones on the first start
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status != Git_OK)
	{
		LogError("Cannot open %s: %s", CONFIGS_URL, error);
		return;
	}
	g_Repo = repo;
	g_Repo.Watch(CONFIGS_BRANCH);              // fetched every watch_interval seconds
}

// Every plugin gets every watch's moves: filter by URL and ref.
public void Git_OnRefChanged(const char[] url, const char[] ref, const char[] oldSha, const char[] newSha)
{
	if (g_Repo == null || !StrEqual(ref, CONFIGS_BRANCH))
		return;

	char ourUrl[PLATFORM_MAX_PATH];
	g_Repo.GetUrl(ourUrl, sizeof(ourUrl));
	if (!StrEqual(url, ourUrl))
		return;

	if (newSha[0] == '\0')
	{
		LogError("The branch %s is gone", CONFIGS_BRANCH);
		return;
	}

	// Write the repository's cfg/ into tf/cfg/. With oldSha, files the
	// branch dropped since then are deleted too.
	g_Repo.WriteDir(newSha, "cfg", "cfg", OnWritten, 0, oldSha);
}

void OnWritten(GitStatus status, GitChanges changes, const char[] error, any data)
{
	// changes is valid whatever the status: what was done before a failure.
	if (status != Git_OK)
		LogError("Cannot write all configs: %s", error);

	char path[PLATFORM_MAX_PATH];
	for (int i = 0; i < changes.Length; i++)
	{
		changes.GetPath(i, path, sizeof(path));
		LogMessage("%s %s", changes.GetKind(i) == GitChange_Written ? "Updated" : "Deleted", path);
	}
	delete changes;
}
```

A commit that adds `cfg/motd.cfg` was pushed while the plugin ran. At the watch's next fetch, the log shows:

```
[follow.smx] Updated cfg/motd.cfg
```

How it behaves:

- `Git_OnRefChanged` can come before `OnOpened`, for another plugin's watch. Hence the `g_Repo == null` check.
- The first fetch of a new watch only notes where `main` is. It fires no forward, so nothing is written at start.
- `Git_OnRefChanged` fires when `main` is at another commit than at the last fetch.
- `WriteDir` writes only files whose content differs. Unchanged files are skipped and not listed.
- A push that deletes `cfg/` entirely makes `WriteDir` fail with `Git_NotFound`. Nothing is deleted then.

What a restart misses: pushes made while the server was down are fetched by the first fetch, which fires no forward.
To catch up, save the last `newSha` you wrote (a file, a database), and in `OnOpened` call
`g_Repo.WriteDir(CONFIGS_BRANCH, "cfg", "cfg", OnWritten, 0, savedSha)`. Use the branch name, not `HEAD`: `HEAD` is the
default branch, which need not be the watched one. If that ends with `Git_NotFound` (the saved SHA is gone, such as
after a force push and a new clone), call it again without `oldRev`.

## Read one file

`sm_motd_preview [rev]` prints `motd.txt` of a revision without writing it anywhere.

```sourcepawn
#include <sourcemod>
#include <git>

// ReadFile copies the content onto the plugin's heap: make room for it.
#pragma dynamic 65536

#define MAX_MOTD 32768

public void OnPluginStart()
{
	RegAdminCmd("sm_motd_preview", Command_Preview, ADMFLAG_CONFIG, "sm_motd_preview [rev]");
}

Action Command_Preview(int client, int args)
{
	char rev[64] = "HEAD";
	if (args >= 1)
		GetCmdArg(1, rev, sizeof(rev));

	// Opening is cheap once the repository is cloned.
	DataPack pack = new DataPack();
	pack.WriteCell(client == 0 ? 0 : GetClientUserId(client));
	pack.WriteString(rev);
	GitRepo.Open("https://github.com/owner/motd", OnOpened, pack);
	return Plugin_Handled;
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, DataPack pack)
{
	if (status != Git_OK)
	{
		LogError("Cannot open the repository: %s", error);
		delete pack;
		return;
	}
	pack.Reset();
	pack.ReadCell();
	char rev[64];
	pack.ReadString(rev, sizeof(rev));

	repo.ReadFile(rev, "motd.txt", OnRead, pack, MAX_MOTD);
	delete repo;                                // queued work keeps running
}

void OnRead(GitStatus status, const char[] content, int length, const char[] error, DataPack pack)
{
	pack.Reset();
	int userid = pack.ReadCell();
	delete pack;
	int client = userid == 0 ? 0 : GetClientOfUserId(userid);
	if (userid != 0 && client == 0)
		return;                                 // the admin left

	switch (status)
	{
		case Git_OK:
		{
			PrintToConsole(client, "motd.txt has %d bytes:", length);
			PrintInPieces(client, content, length);
		}
		case Git_NotFound: PrintToConsole(client, "No such revision, or no motd.txt in it.");
		case Git_TooLarge: PrintToConsole(client, "motd.txt is larger than %d bytes.", MAX_MOTD);
		default:           PrintToConsole(client, "Cannot read motd.txt: %s", error);
	}
}

// PrintToConsole cuts lines at about 1000 bytes: print long text in pieces.
// (PrintToConsole with client 0 prints to the server console.)
void PrintInPieces(int client, const char[] text, int length)
{
	char piece[900];
	for (int start = 0; start < length; start += sizeof(piece) - 1)
	{
		strcopy(piece, sizeof(piece), text[start]);
		PrintToConsole(client, "%s", piece);
	}
}
```

| Command | Prints |
|---|---|
| `sm_motd_preview` | `motd.txt has 23 bytes:` and `Welcome to our server!` |
| `sm_motd_preview v9` | `No such revision, or no motd.txt in it.` |

- `#pragma dynamic` is needed. Without it, a plugin has about 16 KB for stack and heap together.
- `PrintToConsole` and `PrintToServer` cut a line at about 1000 bytes, hence `PrintInPieces`.
- `content` may hold zero bytes (binary files). Use `length`, not `strlen`.
- Pass the user id, not the client index, through callbacks. The player may leave before the answer.
- Deleting the `GitRepo` handle does not cancel the queued `ReadFile`.

## Show recent changes

`sm_changes [days]` lists the commits of the default branch that changed `plugins/`, newest first.

```sourcepawn
#include <sourcemod>
#include <git>

GitRepo g_Repo;

public void OnPluginStart()
{
	GitRepo.Open("https://github.com/owner/plugins", OnOpened);
	RegAdminCmd("sm_changes", Command_Changes, ADMFLAG_GENERIC, "sm_changes [days]: commits to plugins/ of the last days");
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status == Git_OK)
		g_Repo = repo;
	else
		LogError("Cannot open the repository: %s", error);
}

Action Command_Changes(int client, int args)
{
	if (g_Repo == null)
	{
		ReplyToCommand(client, "The repository is not open yet.");
		return Plugin_Handled;
	}
	int days = args >= 1 ? GetCmdArgInt(1) : 7;
	int since = GetTime() - days * 86400;

	// At most 20 commits of the default branch that changed plugins/,
	// authored in the last days.
	int userid = client == 0 ? 0 : GetClientUserId(client);
	g_Repo.Log("HEAD", OnLogged, userid, "plugins", 20, since);
	return Plugin_Handled;
}

void OnLogged(GitStatus status, GitCommitList commits, const char[] error, int userid)
{
	int client = userid == 0 ? 0 : GetClientOfUserId(userid);
	if (userid != 0 && client == 0)
	{
		delete commits;                         // the admin left; delete is safe on null
		return;
	}
	if (status != Git_OK)
	{
		PrintToConsole(client, "Cannot read the history: %s", error);
		return;                                 // commits is null: nothing to delete
	}

	char sha[41], author[64], summary[128], date[32];
	for (int i = 0; i < commits.Length; i++)
	{
		commits.GetSha(i, sha, sizeof(sha));
		commits.GetAuthorName(i, author, sizeof(author));
		commits.GetSummary(i, summary, sizeof(summary));
		FormatTime(date, sizeof(date), "%Y-%m-%d", commits.GetTime(i));
		sha[7] = '\0';                          // short SHA
		PrintToConsole(client, "%s %s %s: %s", sha, date, author, summary);
	}
	if (commits.Length == 0)
		PrintToConsole(client, "No changes.");
	delete commits;
}
```

`sm_changes 3650` on the test repository, whose newer commits changed only files outside `plugins/`:

```
2359fe4 2026-02-01 Tester: second
f71facd 2026-01-01 Tester: first
```

- A result handle is null when the status is not `Git_OK`. Only a valid handle needs `delete`.
- `GetTime(i)` is the author time. `since` compares author times too.

## Install the newest release tag

At start: fetch, find the highest `vMAJOR.MINOR` tag (`v1.10` after `v1.9`), log what changed in `plugins/` since the
installed one, and write the plugin file.

```sourcepawn
#include <sourcemod>
#include <git>

GitRepo g_Repo;
char g_Installed[64];                           // the tag written last time (memory only, in this example)

public void OnPluginStart()
{
	GitRepo.Open("https://github.com/owner/myplugin", OnOpened);
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status != Git_OK)
	{
		LogError("Cannot open the repository: %s", error);
		return;
	}
	g_Repo = repo;
	g_Repo.Fetch(OnFetched);                    // get tags pushed since the clone
}

void OnFetched(GitStatus status, bool changed, const char[] error, any data)
{
	if (status != Git_OK)
		LogError("Fetch failed, using what is on disk: %s", error);
	g_Repo.ListRefs(OnRefs);
}

void OnRefs(GitStatus status, GitRefList refs, const char[] error, any data)
{
	if (status != Git_OK)
	{
		LogError("Cannot list the tags: %s", error);
		return;
	}

	// The highest version number wins: v1.10 after v1.9.
	char name[64], newest[64];
	int newestVersion[2] = {-1, -1};
	for (int i = 0; i < refs.Length; i++)
	{
		if (!refs.IsTag(i))
			continue;
		refs.GetName(i, name, sizeof(name));
		int version[2];
		if (!ParseVersion(name, version))
			continue;
		if (version[0] > newestVersion[0] || (version[0] == newestVersion[0] && version[1] > newestVersion[1]))
		{
			newestVersion = version;
			strcopy(newest, sizeof(newest), name);
		}
	}
	delete refs;

	if (newest[0] == '\0' || StrEqual(newest, g_Installed))
		return;

	if (g_Installed[0] != '\0')
		g_Repo.Diff(g_Installed, newest, OnDiff, 0, "plugins");

	DataPack pack = new DataPack();
	pack.WriteString(newest);
	g_Repo.WriteFile(newest, "plugins/myplugin.smx", "addons/sourcemod/plugins/myplugin.smx", OnWritten, pack);
}

// "v1.2" -> {1, 2}. Anything else ("v1.2.3", "v1.2-rc1", "latest") is no release.
bool ParseVersion(const char[] tag, int version[2])
{
	if (tag[0] != 'v')
		return false;
	char parts[3][16];
	if (ExplodeString(tag[1], ".", parts, sizeof(parts), sizeof(parts[])) != 2)
		return false;
	for (int i = 0; i < 2; i++)
	{
		if (parts[i][0] == '\0' || StringToIntEx(parts[i], version[i]) != strlen(parts[i]))
			return false;
	}
	return true;
}

void OnDiff(GitStatus status, GitDiff diff, const char[] error, any data)
{
	if (status != Git_OK)
		return;
	static const char kinds[][] = {"added", "modified", "deleted"};
	char path[PLATFORM_MAX_PATH];
	for (int i = 0; i < diff.Length; i++)
	{
		diff.GetPath(i, path, sizeof(path));
		LogMessage("%s %s", kinds[diff.GetStatus(i)], path);
	}
	delete diff;
}

void OnWritten(GitStatus status, GitChanges changes, const char[] error, DataPack pack)
{
	pack.Reset();
	char tag[64];
	pack.ReadString(tag, sizeof(tag));
	delete pack;

	if (status == Git_OK)
	{
		strcopy(g_Installed, sizeof(g_Installed), tag);
		if (changes.Length > 0)                 // 0: the file had that content already
			LogMessage("Installed %s", tag);
	}
	else
		LogError("Cannot install %s: %s", tag, error);
	delete changes;
}
```

With the tags `v1.0`, `v1.9` and `v1.10` on the server and `g_Installed` set to `"v1.0"`, the log shows:

```
[release.smx] modified plugins/a.txt
[release.smx] added plugins/b.txt
[release.smx] added plugins/myplugin.smx
[release.smx] Installed v1.10
```

- `GitRepo.Open` fetches nothing when the repository is already cloned. Call `Fetch` to get what was pushed since.
- `Diff` and `WriteFile` run on one worker thread, in the order they were queued.
- `GitChanges` of `WriteFile` is always a valid handle. Delete it in every case.
- `WriteFile` leaves a file alone that already has the content. Then `changes.Length` is 0.

## Check the server's files

`sm_verify` compares each file below `addons/` in the repository's `HEAD` with the same path below `tf/`. It changes
nothing, and it never clones: it only looks at a repository another plugin cloned.

```sourcepawn
#include <sourcemod>
#include <git>

public void OnPluginStart()
{
	RegAdminCmd("sm_verify", Command_Verify, ADMFLAG_ROOT);
}

Action Command_Verify(int client, int args)
{
	// clone = false: only look at a repository another plugin cloned.
	GitRepo.Open("https://github.com/owner/server-files", OnOpened, 0, false);
	return Plugin_Handled;
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status == Git_NotCloned)
	{
		PrintToServer("Not cloned on this server.");
		return;
	}
	if (status != Git_OK)
	{
		PrintToServer("Cannot open: %s", error);
		return;
	}
	repo.ListTree("HEAD", OnTree, repo, "addons");   // the repo travels as data
}

void OnTree(GitStatus status, GitTree tree, const char[] error, GitRepo repo)
{
	if (status != Git_OK)
	{
		PrintToServer("Cannot list addons/: %s", error);
		delete repo;
		return;
	}

	char path[PLATFORM_MAX_PATH];
	for (int i = 0; i < tree.Length; i++)
	{
		if (tree.GetType(i) != GitEntry_File)
			continue;
		tree.GetPath(i, path, sizeof(path));     // "addons/...", the same path below tf/
		DataPack pack = new DataPack();
		pack.WriteString(path);
		repo.CompareFile("HEAD", path, path, OnCompared, pack);
	}
	delete tree;
	delete repo;                                 // queued work still runs
}

void OnCompared(GitStatus status, GitFileState state, const char[] error, DataPack pack)
{
	pack.Reset();
	char path[PLATFORM_MAX_PATH];
	pack.ReadString(path, sizeof(path));
	delete pack;

	if (status != Git_OK)
		PrintToServer("%s: cannot compare: %s", path, error);
	else if (state == GitFile_Missing)
		PrintToServer("%s: missing", path);
	else if (state == GitFile_Different)
		PrintToServer("%s: changed on the server", path);
}
```

On a server where nothing cloned it:

```
Not cloned on this server.
```

Once cloned, run with `"plugins"` in place of `"addons"` against the test repository, on a server that has none of
the files:

```
plugins/a.txt: missing
plugins/b.txt: missing
plugins/myplugin.smx: missing
```

- `ListTree` lists recursively by default. Each directory comes before its own entries.
- `tree.GetPath` is the path from the repository's top, not from the listed directory.
- `CompareFile` reads the server file on a worker thread. A symlink or anything but a regular file counts as different.

## List the repositories

`sm_repos` prints one line per repository below the root, whoever cloned it.

```sourcepawn
#include <sourcemod>
#include <git>

// sm_repos: one line per repository below the root.

public void OnPluginStart()
{
	RegAdminCmd("sm_repos", Command_Repos, ADMFLAG_ROOT);
}

Action Command_Repos(int client, int args)
{
	Git_ListRepositories(OnListed);
	return Plugin_Handled;
}

void OnListed(GitStatus status, GitRepoList repos, const char[] error, any data)
{
	if (status != Git_OK)
	{
		PrintToServer("Cannot read the root: %s", error);
		return;
	}
	char name[PLATFORM_MAX_PATH], branch[64], sha[41], openers[256], watchers[256];
	for (int i = 0; i < repos.Length; i++)
	{
		repos.GetName(i, name, sizeof(name));
		repos.GetHeadBranch(i, branch, sizeof(branch));
		repos.GetHeadSha(i, sha, sizeof(sha));
		repos.GetOpeners(i, openers, sizeof(openers));
		repos.GetWatchers(i, watchers, sizeof(watchers));
		char fetched[32] = "never";
		if (repos.GetFetchTime(i) != 0)
			Format(fetched, sizeof(fetched), "%d min ago", (GetTime() - repos.GetFetchTime(i)) / 60);
		PrintToServer("%s  %s %.7s  fetched %s  open: %s  watching: %s",
		              name, branch, sha, fetched, openers, watchers);
	}
	delete repos;
}
```

With one repository that another plugin, `updater.smx`, has open and watches:

```
localhost:8443/test/plugins  main 95ee09e  fetched 0 min ago  open: updater.smx  watching: main (60 s)
```

`sm_git list` of the `git-admin` plugin prints the same, formatted. See [admin.md](admin.md#sm_git).

## Remove a repository

`sm_forget` deletes a repository from disk, for every server sharing the root.

```sourcepawn
#include <sourcemod>
#include <git>

GitRepo g_Repo;

public void OnPluginStart()
{
	GitRepo.Open("https://github.com/owner/configs", OnOpened);
	RegAdminCmd("sm_forget", Command_Forget, ADMFLAG_ROOT, "Deletes the configs repository from disk");
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status != Git_OK)
		return;
	g_Repo = repo;
	g_Repo.Watch("main");
}

Action Command_Forget(int client, int args)
{
	if (g_Repo == null)
		return Plugin_Handled;
	g_Repo.Unwatch("main");                      // our own watch would block the removal
	g_Repo.Remove(OnRemoved);
	return Plugin_Handled;
}

void OnRemoved(GitStatus status, const char[] error, any data)
{
	if (status == Git_OK)
	{
		PrintToServer("Removed.");
		delete g_Repo;                           // the handle stays valid, but the repository is gone
		return;
	}
	if (status == Git_InUse)
		PrintToServer("Still in use: %s", error);   // names the plugins
	else
		PrintToServer("Cannot remove: %s", error);
	g_Repo.Watch("main");                        // keep following it; sm_forget may be retried
}
```

| Plugin | `sm_forget` prints |
|---|---|
| as shown | `Removed.` |
| without the `Unwatch` line | `Still in use: in use by forget.smx` |

- On any failure the plugin watches again and keeps its handle, so the command can be retried.
- `Remove` is refused while any plugin of this server watches the repository, has it open through another handle, or
  is opening it.
- Plugins of other servers sharing the root are not asked.

# sm-git plugin API

Everything a plugin can do with the `git` extension. Include `<git>`. The library is `git`, the extension file
`git.ext`.

- New here? Start with the [README](../README.md), then [examples.md](examples.md).
- Settings, `sm_git`, shared roots and crashes: [admin.md](admin.md).

Contents:

- [Terms](#terms)
- [How calls work](#how-calls-work)
- [GitStatus](#gitstatus)
- [Names: URLs, revisions, paths](#names-urls-revisions-paths)
- [GitRepo](#gitrepo): [Open](#gitrepoopen), [Fetch](#gitrepofetch), [Resolve](#gitreporesolve),
  [ListRefs](#gitrepolistrefs), [Log](#gitrepolog), [Diff](#gitrepodiff), [ListTree](#gitrepolisttree),
  [ReadFile](#gitreporeadfile), [WriteFile](#gitrepowritefile), [WriteDir](#gitrepowritedir),
  [CompareFile](#gitrepocomparefile), [Watch](#gitrepowatch), [Unwatch](#gitrepounwatch),
  [Remove](#gitreporemove), [GetUrl, GetName](#gitrepogeturl-gitrepogetname)
- [Git_ListRepositories](#git_listrepositories)
- [Git_OnRefChanged](#git_onrefchanged)
- [Result handles](#result-handles)
- [Write-out rules](#write-out-rules)
- [Watches](#watches)
- [Limits](#limits)

## Terms

| Term | Meaning |
|---|---|
| remote | A git repository on a server, named by its `https://` URL. |
| repository | The bare clone of a remote on the game server's disk, with every branch and tag. |
| root | The directory that holds every repository, each at `<host>[:<port>]/<path>.git` below it. Several servers may share it. |
| revision | Anything git resolves to a commit: a branch, a tag, a SHA, `HEAD`, `main~2`. |
| ref | A branch or a tag. |
| write-out | Copying a file or directory of a revision into the game directory. |
| change | One file a write-out wrote or deleted. |
| watch | A plugin's request to have a repository fetched on a timer and be told when a ref moved. |
| watcher | A plugin that has a watch on a ref of a repository. |
| leftover | What a crashed server left below the root: an interrupted removal or an abandoned clone temp directory. |

## How calls work

```sourcepawn
GitRepo.Open("https://github.com/owner/configs", OnOpened);

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status != Git_OK)
	{
		LogError("%s", error);
		return;
	}
	repo.ListRefs(OnRefs);
	delete repo;
}
```

- **Everything is asynchronous.** Every native that does git work queues it and returns at once. The answer comes
  in a callback on a later game frame. The game thread never touches the disk or the network.
- **Two worker threads**, each in order: one for clones, fetches and removals, one for everything else. A slow clone
  never delays reads or write-outs.
- **Callback layout**: a `GitStatus` first, the payload, then an error text and your `data` last.
- **Result handles** are valid only with `Git_OK`, and null otherwise. Delete every valid one. Exception: the
  `GitChanges` of `WriteFile` and `WriteDir` is always valid.
- **`GitRepo` handles** belong to the plugin. Delete them when done. Deleting one does not cancel work queued through
  it: its callbacks still come.
- **A plugin that unloads** before its work ends gets no callback. The work still runs to its end. (Unloading the
  extension instead drops queued work: see [admin.md](admin.md#unloading).)
- **Native errors**: invalid arguments raise a native error at the call, not a status. Examples: an invalid handle or
  callback, an empty revision, a path that breaks the [path rules](#paths-in-a-repository). Each native lists its
  own.
- **Getters** that fill a buffer return the bytes written. Getters of result handles raise a native error for an
  invalid handle, a handle of another kind, or an index outside 0 to `Length - 1`.
- **Binary content**: buffers and strings are UTF-8 text, except `ReadFile`'s content, which may hold zero bytes.

## GitStatus

| Status | Meaning |
|---|---|
| `Git_OK` | Success. |
| `Git_NotCloned` | The repository is not on disk, or no longer. |
| `Git_NotFound` | No such revision, or no such path in the commit. For a file: also a path that is a directory or a symlink. |
| `Git_Refused` | Not allowed. A URL that is not accepted, also a stored one at a fetch ([URL rules](#remote-urls)). A destination inside the root or out of `tf/`. A `*.git-tmp` part in a write-out. Unsafe names or too deep directories in a listing ([refused content](#refused-repository-content)). |
| `Git_Network` | The remote could not be reached, answered wrongly, failed the certificate check (private CA: `ca_file` in [admin.md](admin.md#settings)), or redirected to another host or scheme. |
| `Git_Timeout` | The remote took too long. See `connect_timeout` and `stall_timeout` in [admin.md](admin.md#settings). |
| `Git_TooLarge` | Beyond a [limit](#limits): the size cap, 10000 refs, `maxBytes`, a listing or directory too large. |
| `Git_InUse` | `Remove` refused: a plugin has the repository open, is opening it or watches it. |
| `Git_Failed` | Anything else. The error text tells. |

The error text is meant for logs and admins. Do not parse it.

## Names: URLs, revisions, paths

### Remote URLs

`https://<host>[:<port>]/<path>`, with or without `.git` at the end.

| Rule | Refused example |
|---|---|
| `https://` only, in lower case | `http://...`, `ssh://...`, `git://...`, `git@host:path`, `HTTPS://...`, a local path |
| A plain host name: letters, digits, `.`, `-`; not starting with `.` or `-`; not ending in `.git` | `https://user@host/x`, `https://ex_ample.com/x` |
| An optional port of 1 to 5 digits | `https://host:123456/x` |
| A path of letters, digits, `.`, `-`, `_`, `~` and `/` | `https://host/a?b`, `https://host/a#b`, `https://host/a%20b` |
| No empty, `.` or `..` parts | `https://host/a//b`, `https://host/a/../b` |
| No part ending in `.git` but the last, and no last part of only `.git` | `https://host/a.git/b`, `https://host/owner/.git` |

A refused URL ends with `Git_Refused`, not a native error.

Redirects are followed only on the same host (another path or port). See [admin.md](admin.md#security).

A URL is normalized: host in lower case, without a trailing `/` and without `.git`. The path keeps its case.
`https://github.com/Owner/Repo` and `https://github.com/owner/repo` are two repositories and two clones, although most
hosts serve both. Write a remote's URL the same way everywhere.

| Written | `GetUrl` | `GetName` |
|---|---|---|
| `https://GitHub.com/owner/configs.git/` | `https://github.com/owner/configs` | `github.com/owner/configs` |
| `https://git.example.com:8443/team/cfg` | `https://git.example.com:8443/team/cfg` | `git.example.com:8443/team/cfg` |

### Revisions

Anything git resolves to a commit: a branch (`main`), a tag (`v1.0`), a SHA (full or abbreviated), `HEAD` (the
default branch), an expression (`main~2`, `v1.0^`). An empty revision is a native error.

A name that is both a branch and a tag resolves to the tag, as in git.

### Paths in a repository

Relative to the repository's top. `""` is the top itself.

- Parts are separated by single `/`. A trailing `/` is dropped.
- No leading `/`, no empty (`a//b`), `.` or `..` parts.
- No `.git` part, in any case.
- A path that breaks a rule is a native error.

### Paths on the server

Relative to the game directory (`tf/`), with the same rules as above, and also:

- never empty;
- no backslashes.

Where a path on the server leads, symlinks resolved, is checked when the work runs. See
[Write-out rules](#write-out-rules).

## GitRepo

A handle on one repository. Get one from `GitRepo.Open`.

### `GitRepo.Open`

```sourcepawn
static void Open(const char[] url, GitRepoCallback callback, any data = 0, bool clone = true)

void (GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
```

Opens the repository of a remote. Clones it first when it is not below the root.

| Parameter | Meaning |
|---|---|
| `url` | The remote. See [Remote URLs](#remote-urls). A refused URL ends with `Git_Refused`. |
| `clone` | False: never clone. A missing repository ends with `Git_NotCloned`. For plugins that only look at what another one cloned. |

| Callback gets | Meaning |
|---|---|
| `repo` | The handle, with `Git_OK`; null otherwise. Delete it when done. |
| `cloned` | True when this call cloned the repository, also when another server's clone at the same moment won the race. |

- Opening an existing repository fetches nothing. Call [`Fetch`](#gitrepofetch) for news.
- A clone has every branch and tag and the whole history.
- An `Open` called after a `Remove` that passed the in-use check waits for the removal, then answers `Git_NotCloned`
  or clones anew.
- Native error: an invalid callback.

### `GitRepo.Fetch`

```sourcepawn
void Fetch(GitFetchCallback callback, any data = 0)

void (GitStatus status, bool changed, const char[] error, any data)
```

Fetches every branch and tag from the remote.

- Drops branches and tags the remote no longer has.
- Follows the remote's default branch (`HEAD`).
- `changed`: a branch or tag moved, appeared or went away, or the default branch changed.
  Always false unless `Git_OK`.
- Watched refs that moved are told through [`Git_OnRefChanged`](#git_onrefchanged) before the callback. The refs on
  disk are compared also when the fetch failed.
- Checks again that the repository's stored remote URL is accepted. If not: `Git_Refused`.
- A fetch whose download would take the repository past the size cap fails with `Git_TooLarge`. A fetch with nothing
  to download still succeeds. See [Limits](#limits).
- Native errors: an invalid handle or callback.

### `GitRepo.Resolve`

```sourcepawn
void Resolve(const char[] rev, GitResolveCallback callback, any data = 0)

void (GitStatus status, const char[] sha, const char[] error, any data)
```

The full SHA of the commit a revision names. `Git_NotFound` when there is none.

Native errors: an invalid handle or callback, an empty `rev`.

### `GitRepo.ListRefs`

```sourcepawn
void ListRefs(GitRefsCallback callback, any data = 0)

void (GitStatus status, GitRefList refs, const char[] error, any data)
```

The branches and tags, and the default branch. See [GitRefList](#gitreflist).

Native errors: an invalid handle or callback.

### `GitRepo.Log`

```sourcepawn
void Log(const char[] rev, GitLogCallback callback, any data = 0, const char[] path = "", int max = 50, int since = 0)

void (GitStatus status, GitCommitList commits, const char[] error, any data)
```

The commits reachable from a revision, newest first. See [GitCommitList](#gitcommitlist).

| Parameter | Meaning |
|---|---|
| `path` | Only commits that changed this file or directory. A merge counts only when it changed the path against every parent. `""`: all commits. |
| `max` | At most this many commits, 1 to 1000. |
| `since` | Only commits authored at or after this Unix timestamp. 0: no limit. |

How `since` reads the history:

- Newest commit first, until the first commit **committed** more than a day before `since`.
- So a large history is not read to its end.
- A commit authored after `since` but committed more than a day before it is not listed.

Native errors: an invalid handle or callback, an empty `rev`, an invalid `path`, a `max` outside 1 to 1000.

### `GitRepo.Diff`

```sourcepawn
void Diff(const char[] base, const char[] head, GitDiffCallback callback, any data = 0, const char[] path = "")

void (GitStatus status, GitDiff diff, const char[] error, any data)
```

The files that differ between two revisions, sorted by path. See [GitDiff](#gitdiff).

- `path`: only this file, or files below this directory. `""`: all files. A path in neither revision gives an empty
  list, not `Git_NotFound`.
- A renamed file is a deleted one and an added one.
- No patch text: only paths, kinds and blob SHAs.

Native errors: an invalid handle or callback, an empty `base` or `head`, an invalid `path`.

### `GitRepo.ListTree`

```sourcepawn
void ListTree(const char[] rev, GitTreeCallback callback, any data = 0, const char[] path = "", bool recursive = true)

void (GitStatus status, GitTree tree, const char[] error, any data)
```

The entries of a directory at a revision. See [GitTree](#gittree).

| Parameter | Meaning |
|---|---|
| `path` | The directory. `""`: the top. `Git_NotFound` when the commit has no directory there. |
| `recursive` | Also the entries of its directories. Each directory comes before its own entries. |

- More than 100000 entries, or more than 32 MB of paths, end with `Git_TooLarge`.
- Unsafe names or too deep directories below `path` end with `Git_Refused`. See
  [Refused repository content](#refused-repository-content).

Native errors: an invalid handle or callback, an empty `rev`, an invalid `path`.

### `GitRepo.ReadFile`

```sourcepawn
void ReadFile(const char[] rev, const char[] path, GitReadCallback callback, any data = 0, int maxBytes = 4096)

void (GitStatus status, const char[] content, int length, const char[] error, any data)
```

The content of a file at a revision.

- `maxBytes`: a larger file ends with `Git_TooLarge`, checked before the content is unpacked.
- `content` holds `length` bytes. A binary file may have zero bytes inside: use `length`, not `strlen`.
- `Git_NotFound`: no such revision, or no such file in it. A directory or a symlink at `path` is no file.

**Heap size.** The content is copied onto the plugin's heap for the callback. A plugin has about 16 KB for stack and
heap together unless it says otherwise, hence the default of 4096. To read larger files, give the plugin more room:

```sourcepawn
#pragma dynamic 131072      // cells, so 512 KB of stack and heap
```

The test plugin reads files of 40000 bytes with this setting.

Native errors: an invalid handle or callback, an empty `rev`, an invalid or empty `path`, a `maxBytes` below 1.

### `GitRepo.WriteFile`

```sourcepawn
void WriteFile(const char[] rev, const char[] path, const char[] dest, GitWriteCallback callback, any data = 0)

void (GitStatus status, GitChanges changes, const char[] error, any data)
```

Writes one file of a revision to the server.

| Parameter | Meaning |
|---|---|
| `path` | The file in the repository. |
| `dest` | The file on the server, relative to `tf/`. Missing directories are created. |

- Writes only when `dest` lacks that content. Then `changes` has one entry, else none.
- `Git_NotFound` when `path` is no file at `rev`: missing, a directory or a symlink.
- `changes` is always a valid handle, also on failure (then empty). Delete it.
- All [write-out rules](#write-out-rules) apply.

Native errors: an invalid handle or callback, an empty `rev`, an invalid or empty `path` or `dest`.

### `GitRepo.WriteDir`

```sourcepawn
void WriteDir(const char[] rev, const char[] path, const char[] destDir, GitWriteCallback callback, any data = 0,
              const char[] oldRev = "")

void (GitStatus status, GitChanges changes, const char[] error, any data)
```

Writes every file of a directory at a revision below a directory on the server, each like `WriteFile`.

| Parameter | Meaning |
|---|---|
| `path` | The directory in the repository. `""`: the whole repository. |
| `destDir` | The directory on the server, relative to `tf/`. |
| `oldRev` | The revision written there before, or `""`. |

Deleting:

- With `oldRev`: files that `oldRev` had in the directory and `rev` no longer has are deleted on the server, before
  anything is written. They are deleted whatever their content on the server.
- `oldRev` may lack the directory. Then nothing is deleted.
- An `oldRev` that does not resolve ends with `Git_NotFound`. Nothing is deleted or written.
- Without `oldRev`: nothing is ever deleted.
- Files the repository never had are never touched.
- Directories emptied by deletions stay.
- `Git_NotFound` when `rev` has no such directory. Nothing is deleted or written then.

Partial failure:

- Files are written one by one. When one fails, the callback gets the failure **and** the changes made before it.
  Those stay.
- Nothing at all is written when: the files together exceed the size cap, the directory has more than 100000 entries
  (`Git_TooLarge`), or `destDir` or a path of `rev` or `oldRev` has a `*.git-tmp` part (`Git_Refused`).

Symlinks and submodules are left out. All [write-out rules](#write-out-rules) apply.

Native errors: an invalid handle or callback, an empty `rev`, an invalid `path` or `destDir`.

### `GitRepo.CompareFile`

```sourcepawn
void CompareFile(const char[] rev, const char[] path, const char[] file, GitCompareCallback callback, any data = 0)

void (GitStatus status, GitFileState state, const char[] error, any data)
```

Tells whether a file on the server has the content of a file at a revision.

| `GitFileState` | Meaning |
|---|---|
| `GitFile_Same` | Exactly the commit's content. |
| `GitFile_Different` | Other content, or not a regular file (a symlink is never followed). Also whenever the status is not `Git_OK`. |
| `GitFile_Missing` | No such file on the server. |

- `path`: the file in the repository. `Git_NotFound` when the commit has none there.
- `file`: the file on the server, relative to `tf/`. Checked like a write-out's destination: `Git_Refused` inside the
  root or, through a symlink, outside `tf/`.

Native errors: an invalid handle or callback, an empty `rev`, an invalid or empty `path` or `file`.

### `GitRepo.Watch`

```sourcepawn
void Watch(const char[] ref, int interval = 0)
```

Has the repository fetched on a timer, for as long as this plugin is loaded or until `Unwatch`.
[`Git_OnRefChanged`](#git_onrefchanged) is called whenever the ref is at another commit than when it was last seen.

| Parameter | Meaning |
|---|---|
| `ref` | `HEAD` (the default branch), or the short name of a branch or tag: `main`, `release/1.0`, `v1.0`. It need not exist yet. |
| `interval` | Seconds between fetches. 0: `watch_interval` of the settings. Below 60 is raised to 60. It may be longer than `watch_interval`. |

- Watching the same ref again changes this plugin's interval.
- The watch belongs to the plugin, not to the handle. Deleting the handle does not end it, and a watch blocks
  `Remove`. Call `Unwatch` before deleting the handle, or keep the handle.
- See [Watches](#watches) for timers, sharing and ref names.
- Native errors: an invalid handle; a `ref` that is no such name (`""`, `refs/heads/main`, `main~1`, `a..b`); a
  negative `interval`.

### `GitRepo.Unwatch`

```sourcepawn
void Unwatch(const char[] ref)
```

Ends this plugin's watch of a ref. Nothing happens when it has none.

Native error: an invalid handle.

### `GitRepo.Remove`

```sourcepawn
void Remove(GitStatusCallback callback, any data = 0)

void (GitStatus status, const char[] error, any data)
```

Deletes the repository from disk, **for every server sharing the root**.

- Waits for the work queued before it.
- Refused with `Git_InUse` while a plugin of this server watches the repository, has it open through another handle
  than this one, or is opening it (an `Open` that clones it or waits for an earlier removal). The error text names the
  plugins.
- Plugins of other servers sharing the root are not asked.
- A repository that is not on disk: `Git_NotCloned`.
- An `Open` called after this `Remove` passed the in-use check waits for the removal to end.
- This handle stays valid. Work queued through it afterwards ends with `Git_NotCloned`.

Native errors: an invalid handle or callback.

### `GitRepo.GetUrl`, `GitRepo.GetName`

```sourcepawn
int GetUrl(char[] buffer, int maxlength)     // "https://github.com/owner/name"
int GetName(char[] buffer, int maxlength)    // "github.com/owner/name"
```

- `GetUrl`: the normalized remote URL, without `.git`. The same text `Git_OnRefChanged` gives.
- `GetName`: `<host>[:<port>]/<path>`, the repository's place below the root without `.git`.
- Both return the bytes written. Native error: an invalid handle.

## `Git_ListRepositories`

```sourcepawn
void Git_ListRepositories(GitRepoListCallback callback, any data = 0)

void (GitStatus status, GitRepoList repos, const char[] error, any data)
```

Lists the repositories below the root, whoever cloned them. See [GitRepoList](#gitrepolist).

- Directories below the root that cannot be read are left out.
- A root that does not exist yet: `Git_OK` with an empty list.
- `Git_Failed` when the root exists but cannot be read.
- Native error: an invalid callback.

## `Git_OnRefChanged`

```sourcepawn
forward void Git_OnRefChanged(const char[] url, const char[] ref, const char[] oldSha, const char[] newSha);
```

Called when a watched ref is at another commit than when it was last seen.

| Parameter | Meaning |
|---|---|
| `url` | The repository's normalized URL, as `GetUrl` gives it. |
| `ref` | The ref as it was named in `Watch`. |
| `oldSha` | The commit it was at. Empty: it did not exist. |
| `newSha` | The commit it is at. Empty: it is gone. |

- Every plugin with this forward gets every watch's moves, also those of other plugins. Compare `url` and `ref` with
  your own. It may come before your own `Open` has answered.
- It fires for this server's fetches and for those of another server sharing the root.
- Use `newSha`, not the ref's name, for what you do next. The name may resolve to a tag of the same name, or may have
  moved again.

## Result handles

All are handles. Delete them. `Length` is the number of entries. Index from 0 to `Length - 1`.

### `GitRefList`

The branches and tags of a repository at one moment.

| Member | Returns |
|---|---|
| `Length` | The number of refs. |
| `GetName(index, buffer, maxlength)` | The short name: `main`, `v1.0`. |
| `GetSha(index, buffer, maxlength)` | The commit it points to. |
| `IsTag(index)` | True for a tag, false for a branch. |
| `GetHeadBranch(buffer, maxlength)` | The default branch. Empty in an empty repository. |
| `GetHeadSha(buffer, maxlength)` | The commit of the default branch. |

### `GitCommitList`

Commits, newest first.

| Member | Returns |
|---|---|
| `Length` | The number of commits. |
| `GetSha(index, buffer, maxlength)` | The commit's SHA. |
| `GetAuthorName(index, buffer, maxlength)` | The author's name. |
| `GetAuthorEmail(index, buffer, maxlength)` | The author's email address. |
| `GetTime(index)` | The author time, as a Unix timestamp. |
| `GetSummary(index, buffer, maxlength)` | The first line of the message. |
| `GetMessage(index, buffer, maxlength)` | The whole message. |

### `GitDiff`

The files that differ between two revisions, sorted by path.

| Member | Returns |
|---|---|
| `Length` | The number of files. |
| `GetPath(index, buffer, maxlength)` | The file's path from the repository's top. |
| `GetStatus(index)` | `GitDiff_Added`, `GitDiff_Modified` or `GitDiff_Deleted`. |
| `GetOldSha(index, buffer, maxlength)` | The content's blob SHA in `base`. Empty for an added file. |
| `GetNewSha(index, buffer, maxlength)` | The content's blob SHA in `head`. Empty for a deleted file. |

### `GitTree`

The entries of a directory, each directory before its own entries.

| Member | Returns |
|---|---|
| `Length` | The number of entries. |
| `GetPath(index, buffer, maxlength)` | The path from the repository's top, not from the listed directory. |
| `GetType(index)` | `GitEntry_File`, `GitEntry_Directory`, `GitEntry_Symlink` or `GitEntry_Submodule`. |
| `GetSize(index)` | The bytes of a file. 0 for directories and submodules. |
| `GetSha(index, buffer, maxlength)` | The object's SHA. |
| `IsExecutable(index)` | Whether a file has git's executable mode. |

### `GitChanges`

What a write-out changed on the server: deleted files first, then written ones. Files that already had the content
are not in it.

| Member | Returns |
|---|---|
| `Length` | The number of changes. |
| `GetPath(index, buffer, maxlength)` | The file, relative to `tf/`. |
| `GetKind(index)` | `GitChange_Written` or `GitChange_Deleted`. |

### `GitRepoList`

The repositories below the root.

| Member | Returns |
|---|---|
| `Length` | The number of repositories. |
| `GetName(index, buffer, maxlength)` | `<host>[:<port>]/<path>`: `github.com/owner/name`. |
| `GetUrl(index, buffer, maxlength)` | The normalized remote URL. |
| `GetHeadBranch(index, buffer, maxlength)` | The default branch. |
| `GetHeadSha(index, buffer, maxlength)` | Its commit. Empty for an empty repository. |
| `GetFetchTime(index)` | The last fetch by any server sharing the root, as a Unix timestamp. 0 when unknown. |
| `GetOpeners(index, buffer, maxlength)` | The plugins of this server that have it open: `a.smx, b.smx`. Empty for none. |
| `GetWatchers(index, buffer, maxlength)` | The refs watched on this server: `main (300 s)`. Empty for none. |

## Write-out rules

`WriteFile` and `WriteDir` copy files from a repository into the game directory. These rules always hold. A
`CompareFile` target is checked by the same bounds.

### Where

- Destinations are relative to `tf/` and cannot leave it.
- Nothing is written or deleted inside the root, so one repository's content cannot change another repository.
- Both bounds are checked where the destination's directory really leads, symlinks resolved. A symlink below `tf/`
  that leads out of it, or into the root, is refused (`Git_Refused`), never followed.

### How

- Each file is written to `<file>.git-tmp`, then renamed onto the destination. No file is ever half-written.
- The temp file is always created anew. Whatever has its name, a symlink too, is deleted first, never written
  through.
- A file that already has the content is left alone. Only a regular file can have it: a symlink there is replaced,
  never followed.
- An empty directory at a file's destination is removed and replaced by the file.
- Files get mode 0644, executable files 0755.
- Symlinks and submodules of the repository are not written.

### The `.git-tmp` names

A path with a file or directory whose name ends in `.git-tmp` is never written or deleted. Writing `a` would replace
or trip over `a.git-tmp`. So `Git_Refused`, before anything is touched, for:

- a destination with such a part;
- a `WriteDir` where such a name is in `rev` or `oldRev`.

### Refused repository content

`ListTree` and `WriteDir` end with `Git_Refused`, before anything is written, when the listed directory contains:

- a file name that could leave its directory or plant a repository: `.git` in any case, `.`, `..`, a `/`;
- directories nested deeper than 64 levels, counted from the listed directory.

`WriteFile`, `ReadFile` and `CompareFile` look up one path and do not check the rest of the tree.

### Size

- A file larger than the size cap is not written.
- A directory whose files together exceed the size cap is not written at all.
- Nor one with more than 100000 entries. Git trees may share directories, so a small repository can name very many
  files.

### What the plugin learns

The callback gets a `GitChanges` list of what was written and deleted, so the plugin knows what to reload. Also when
the write-out failed partway: then with what was done before the failure.

## Watches

```sourcepawn
repo.Watch("main");            // every watch_interval seconds (default 300)
repo.Watch("v2", 3600);        // hourly
```

### Timer

- One timer per repository serves all plugins that watch it.
- It runs at the shortest interval any watcher asked for.
- Intervals are at least 60 seconds. They may be longer than `watch_interval`.
- It runs on the system's monotonic clock. Setting the clock does not move it.
- A watch of a ref nobody on this server watched yet fetches at once. That first fetch only notes where the ref is:
  no `Git_OnRefChanged`.
- A watch of a ref already watched (by this or another plugin) keeps the repository's timer. The timer only comes
  sooner when the new interval is shorter than what is left of it.
- Every failed fetch of a watch is logged as an error: `Polling <name> failed: <reason>`.

### What moves

- `Git_OnRefChanged` fires when the ref is at another commit than at the last look.
- That includes a ref that appears (`oldSha` empty) and one that goes away (`newSha` empty).
- A watch also sees what another server sharing the root fetched.

### Ref names

- `HEAD`, or a name a branch or tag could have: `main`, `release/1.0`, `v1.0`.
- Not a full ref name (`refs/heads/main`) and not an expression (`main~1`, `a..b`). Those are native errors.
- A name that is a branch and a tag at once follows the **branch**. But a revision of that name (`Resolve`, `Log`,
  ...) resolves to the **tag**, as in git. Use the SHA `Git_OnRefChanged` gives.

### Hibernation

An empty server that hibernates (`tf_allow_server_hibernation 1`) keeps running game frames. Callbacks arrive and
watches fetch on time there too. Checked on the 32-bit and 64-bit test servers, empty, without bots and SourceTV.

## Limits

| Limit | Value | When exceeded |
|---|---|---|
| Size of a repository | `max_size_mb` of the settings, default 200 MB | A clone or fetch whose download would grow past it is aborted: `Git_TooLarge`. |
| Refs on a remote | 10000 branches and tags | Clone and fetch: `Git_TooLarge`. Each ref is a file the size cap barely counts. |
| One object rebuilt from deltas | the size cap | `Git_TooLarge`. |
| Objects a download announces | the size cap in bytes, divided by 8 | `Git_TooLarge`. |
| Entries of one listing (`ListTree`, `WriteDir`) | 100000 | `Git_TooLarge`. |
| Bytes of paths in one listing | 32 MB | `Git_TooLarge`. |
| Directory depth in a repository | 64 levels | `Git_Refused`. |
| One file written out | the size cap | `Git_TooLarge`. |
| One directory written out | the size cap, all files together | `Git_TooLarge`, nothing written. |
| `ReadFile` | `maxBytes` | `Git_TooLarge`. |
| `Log` | 1000 commits | Native error above it. |

Not supported:

- Authentication: public repositories only.
- Git LFS: LFS files stay pointer files.
- Submodules: not followed, not written.
- Remotes in git's SHA-256 object format: cannot be cloned.
- Shallow or partial clones: the whole history is cloned.
- Anything host specific (GitHub releases, pull requests).
- Patch text in diffs, and rename detection.

A repository only grows. See [admin.md](admin.md#a-repository-only-grows) for what to do when it reaches the size cap.

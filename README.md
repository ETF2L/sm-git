# sm-git

The `git` SourceMod extension gives plugins git repositories. It clones and fetches remotes over HTTPS and reads them:
branches and tags, history, differences between commits, directory listings and file contents. It writes files of a
commit into the game directory and watches branches for new commits. The `git-admin` plugin adds the admin command
`sm_git`.

It is made for plugins that keep a server's plugins, extensions and configs in step with a repository: watch a branch,
see what changed, write out only that. It never pushes.

For SourceMod 1.12 on Linux, x86 and x86_64.

## Example

Keeps the server's `cfg/` in step with the `cfg/` of a branch:

```sourcepawn
#include <sourcemod>
#include <git>

GitRepo g_Repo;

public void OnPluginStart()
{
	GitRepo.Open("https://github.com/owner/configs", OnOpened);   // clones on the first start
}

void OnOpened(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data)
{
	if (status != Git_OK)
	{
		LogError("Cannot open the repository: %s", error);
		return;
	}
	g_Repo = repo;
	g_Repo.Watch("main");              // every watch_interval seconds (configs/git.cfg, default 300)
}

public void Git_OnRefChanged(const char[] url, const char[] ref, const char[] oldSha, const char[] newSha)
{
	char ourUrl[PLATFORM_MAX_PATH];
	if (g_Repo == null || !StrEqual(ref, "main") || newSha[0] == '\0')
		return;
	g_Repo.GetUrl(ourUrl, sizeof(ourUrl));
	if (!StrEqual(url, ourUrl))
		return;                                                   // another plugin's watch

	// Write the new commit's cfg/ into tf/cfg/; delete what it dropped since oldSha.
	g_Repo.WriteDir(newSha, "cfg", "cfg", OnWritten, 0, oldSha);
}

void OnWritten(GitStatus status, GitChanges changes, const char[] error, any data)
{
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

After a push that adds `cfg/motd.cfg`, the log shows `Updated cfg/motd.cfg`. `WriteDir` writes only files whose
content differs. Nothing is written at start: the watch's first fetch only notes where `main` is (see
[Follow a branch](docs/examples.md#follow-a-branch)).

## Features

| Feature | API |
|---|---|
| Clone and fetch public HTTPS remotes, with every branch and tag | [`GitRepo.Open`](docs/reference.md#gitrepoopen), [`Fetch`](docs/reference.md#gitrepofetch) |
| Resolve revisions, list branches and tags | [`Resolve`](docs/reference.md#gitreporesolve), [`ListRefs`](docs/reference.md#gitrepolistrefs) |
| History, also of one path or since a date | [`Log`](docs/reference.md#gitrepolog) |
| Files that differ between two revisions | [`Diff`](docs/reference.md#gitrepodiff) |
| Directory listings and file contents | [`ListTree`](docs/reference.md#gitrepolisttree), [`ReadFile`](docs/reference.md#gitreporeadfile) |
| Write a file or directory into the game directory; only what changed, deletions optional | [`WriteFile`](docs/reference.md#gitrepowritefile), [`WriteDir`](docs/reference.md#gitrepowritedir) |
| Check a server file against a commit | [`CompareFile`](docs/reference.md#gitrepocomparefile) |
| Watch a branch or tag, get a forward when it moves | [`Watch`](docs/reference.md#gitrepowatch), [`Git_OnRefChanged`](docs/reference.md#git_onrefchanged) |
| List and remove repositories | [`Git_ListRepositories`](docs/reference.md#git_listrepositories), [`Remove`](docs/reference.md#gitreporemove) |
| Admin command | [`sm_git`](docs/admin.md#sm_git) |

Properties:

- Asynchronous: every call that does git work answers through a callback on a later frame. The game thread never
  waits for the disk or the network.
- Safe write-outs: never outside `tf/` or into the root (symlinks resolved), never half-written files, never written
  through a symlink at the file itself.
- Several servers may share one directory of repositories.
- Size limits on clones, fetches and write-outs.
- No git or OpenSSL needed on the server: libgit2 and Mbed TLS are linked in.
- Certificates are checked against Mozilla's CA bundle. A private CA is added with `ca_file`
  ([settings](docs/admin.md#settings)).

Not supported: authentication, Git LFS, submodules, SHA-256 repositories, host-specific features (GitHub releases).
See [Limits](docs/reference.md#limits).

## Documentation

| File | What is in it |
|---|---|
| [docs/examples.md](docs/examples.md) | Complete plugins, one task each, with their real output. Start here. |
| [docs/reference.md](docs/reference.md) | Every native, callback, status, rule and limit. |
| [docs/admin.md](docs/admin.md) | Install, settings, `sm_git`, shared roots, crashes, logs. |
| `scripting/include/git.inc` | The API, with a comment on every native. |

## Installing

Unpack the release archive `sm-git-v<version>-linux.tar.gz` into the game folder; on a new server rename
`configs/git.cfg.example` to `configs/git.cfg`, or leave it out for the defaults. The files, the commands and the
settings are in [admin.md](docs/admin.md#install).

## Building

`.github/workflows/ci.yml` builds the extension for both architectures and the plugin on an Ubuntu 22.04 runner and
is the reference for a local build:

1. `deps/fetch.py <dir>` downloads libgit2, Mbed TLS, the CA bundle and CMake at the versions `deps/deps.json` pins
   and checks each against its SHA-256.
2. `deps/build-deps.sh <x86|x86_64> <dir> <prefix>` builds the static libraries for one architecture, with clang and
   that CMake on the `PATH`.
3. AMBuild builds the extension and the unit tests against a pinned SourceMod commit (`SOURCEMOD_COMMIT`), with clang:

   ```
   python extension/configure.py --enable-optimize --targets=x86,x86_64 --sm-path=<sourcemod> --deps-path=<prefixes>
   ambuild
   ```

It runs the unit tests of both architectures, checks that the binaries export only `GetSMExtAPI` and need at most
glibc 2.35, compiles `git-admin.sp` with the spcomp of a pinned SourceMod build (`SOURCEMOD_DROP`) and packs
`sm-git-<label>-linux.tar.gz`, laid out like a game folder:

- `extensions/git.ext.so`, `extensions/x64/git.ext.so`
- `plugins/git-admin.smx`
- `configs/git.cfg.example`, `configs/git/ca-bundle.crt`
- `scripting/`
- `licenses/git/`

The settings come as `git.cfg.example`, so unpacking the archive never overwrites a server's own `git.cfg`. For a
pushed `v*` tag it publishes the archive as a GitHub release of `etf2l/sm-git`, after checking the tag against
`SMEXT_CONF_VERSION` in `extension/src/smsdk_config.h`. Before the archive is uploaded, sm-runner's setup and test
actions (`davenonymous/sm-runner/setup@v0` and `test@v0`) run the SourcePawn tests against the packed extension and
plugin with both `smrunner` and `smrunner64`; a failing test fails the job, so no release is published. A separate job
checks `scripting/` and `tests/` with pawncritic and `pawntidy --check`.

### Tests

The SourcePawn tests run under [sm-runner](https://github.com/davenonymous/sm-runner)'s smrunner, a simulated server:

- `tests/test_git.sp`: every native as a plugin calls it: opening and cloning, refused URLs, revisions, refs, the log,
  diffs, trees, reading, writing out and comparing files, the write-out rules, watches and removal, the repository
  list and the documented native errors.
- `tests/test_git_admin.sp`: the `sm_git` command of `git-admin.sp` and its root flag.

They need the network: every test clones `https://github.com/ETF2L/sm-git` into its own root, and reads the tags
`v1.0.0` (the first commit) and `smrunner-fixture` (the commit that added the tests, whose parent is `v1.0.0`). Keep
both tags where they are. Compile the tests with `scripting/include` and smrunner's `share/include` on the include path,
then run them against the packed archive's `addons/sourcemod`:

```
smrunner test test_git.smx --extensions <dist>/extensions \
    --file <dist>/configs/git/ca-bundle.crt=addons/sourcemod/configs/git/ca-bundle.crt
smrunner test test_git_admin.smx --extensions <dist>/extensions --plugin git-admin.smx \
    --file <dist>/configs/git/ca-bundle.crt=addons/sourcemod/configs/git/ca-bundle.crt
```

`smrunner` loads the x86 extension, `smrunner64` the x64 one.

## Layout

| Path | What |
|---|---|
| `extension/src/repo.*` | Everything git, on libgit2. |
| `extension/src/root.*` | The names below the root (repositories, clone temps, removals) and the walk through it. |
| `extension/src/watches.*` | The watches' schedule and which watched refs moved. |
| `extension/src/remote.*` | Parses URLs and paths. |
| `extension/src/config.*` | The settings. |
| `extension/src/worker.*` | The worker thread. |
| `extension/src/serverlog.*` | What the workers have to log. |
| `extension/src/extension.*`, `extension/src/natives.cpp` | The SourceMod layer. Everything above knows no SourceMod. |
| `extension/tests/` | Unit tests. |
| `tests/` | The SourcePawn tests for smrunner. |
| `extension/tools/contend.cpp` | Clones or fetches one repository into a root, to run several at once on one root. |
| `scripting/` | `include/git.inc` and `git-admin.sp`. |
| `configs/git.cfg` | The settings with their defaults. |
| `deps/` | The pinned sources of libgit2, Mbed TLS, the CA bundle and CMake (`deps.json`), their download (`fetch.py`) and build (`build-deps.sh`). |
| `docs/` | The documentation. |
| `LICENSE.txt`, `licenses/` | The license and the GPL texts. |

## AI disclosure

AI helped build this project. If that bothers you, no hard feelings - this project is just not for you.

## License

sm-git is licensed under the GNU General Public License version 3, with the same exceptions SourceMod grants; see
[LICENSE.txt](LICENSE.txt). libgit2 (GPLv2 with the linking exception) and Mbed TLS (used under the Apache License
2.0) keep their own licenses, and the CA bundle is under the Mozilla Public License 2.0; the release installs the
license texts into `addons/sourcemod/licenses/git/`.

# Running sm-git on a server

For server admins: install, settings, the `sm_git` command, sharing a root between servers, and what happens on
crashes. The plugin API is in [reference.md](reference.md).

Contents:

- [Install](#install)
- [Settings](#settings)
- [sm_git](#sm_git)
- [Security](#security)
- [Sharing a root](#sharing-a-root)
- [Crashes and leftovers](#crashes-and-leftovers)
- [Unloading](#unloading)
- [A repository only grows](#a-repository-only-grows)
- [Logs](#logs)

## Install

Unpack the release archive `sm-git-v<version>-linux.tar.gz` into the game folder (for Team Fortress 2 `tf/`). It
carries the settings as `configs/git.cfg.example`, so unpacking it over an installed version never overwrites the
server's own `configs/git.cfg`. On a new server, rename the example to `git.cfg` to change settings; without a
`git.cfg` the defaults apply:

```
tar -xzf sm-git-v<version>-linux.tar.gz -C <game folder>/tf
cp -n <game folder>/tf/addons/sourcemod/configs/git.cfg.example <game folder>/tf/addons/sourcemod/configs/git.cfg
```

The files:

| File | What |
|---|---|
| `extensions/git.ext.so` | The extension, 32-bit. |
| `extensions/x64/git.ext.so` | The extension, 64-bit. |
| `plugins/git-admin.smx` | The `sm_git` command. Optional. |
| `configs/git.cfg.example` | The settings, with their defaults. |
| `configs/git/ca-bundle.crt` | Mozilla's CA certificates. Required. |
| `scripting/` | `git.inc` and `git-admin.sp`. |
| `licenses/git/` | sm-git's license and the license texts of libgit2 and Mbed TLS. |

- SourceMod 1.12, Linux.
- The server needs neither git nor OpenSSL: libgit2 1.9.7 and Mbed TLS 3.6.7 are linked in.
- A plugin that includes `<git>` loads the extension by itself.

## Settings

`addons/sourcemod/configs/git.cfg`:

```
"Git"
{
	"root"              ""
	"ca_file"           ""
	"watch_interval"    "300"
	"connect_timeout"   "10"
	"stall_timeout"     "30"
	"max_size_mb"       "200"
}
```

| Setting | Default | Meaning |
|---|---|---|
| `root` | `""` | Where the repositories are kept. Relative to the game directory (`tf/`), or absolute. Empty: `addons/sourcemod/data/git`. |
| `ca_file` | `""` | A PEM file of CA certificates trusted besides the shipped bundle, for git servers with a private CA. Relative to `tf/`, or absolute. A missing or unreadable file keeps the extension from loading. |
| `watch_interval` | `300` | Seconds between the fetches of a watch that names no interval. Below 60 is raised to 60. |
| `connect_timeout` | `10` | Seconds to reach a remote. 1 to 3600. |
| `stall_timeout` | `30` | Seconds a remote may send nothing. 1 to 3600. |
| `max_size_mb` | `200` | The size cap of one repository, in megabytes. |

- Read when the extension loads. A change needs the extension reloaded.
- Every setting is optional. A missing file means all defaults.
- Numbers are whole and positive, at most nine digits.
- An unknown setting, a bad value or a broken file keeps the extension from loading. The reason is in SourceMod's error
  log.
- With a `ca_file`, the extension writes both certificate files into `addons/sourcemod/data/git-ca-bundle.crt` and
  uses that.

## sm_git

The `git-admin` plugin's command, for admins with the root flag (`z`).

| Command | Does |
|---|---|
| `sm_git list` | Lists the repositories below the root, who has them open and what they watch. |
| `sm_git status <repo>` | One repository with its URL, head, last fetch, users, branches and tags. |
| `sm_git log <repo> [rev] [count]` | Its newest commits. `rev` defaults to `HEAD`, `count` to 10 (1 to 50). |
| `sm_git fetch <repo>` | Fetches it now. |
| `sm_git clone "<url>"` | Clones a repository. |
| `sm_git remove <name\|url>` | Deletes it from disk, unless a plugin of this server uses it. |

Naming a repository:

- `<repo>` is a repository's name (`github.com/owner/name`), its URL, or any part of the name that only one
  repository has.
- `remove` takes only the exact name or URL. It deletes the repository for every server sharing the root.
- `status`, `log`, `fetch` and `remove` never clone. A repository another server removed meanwhile is answered as not
  cloned.

Quoting:

- Put URLs, and names with a port, in quotes: `sm_git clone "https://github.com/owner/name"`,
  `sm_git remove "example.com:8443/owner/name"`.
- The console splits words at `:` and takes `//` as the start of a comment. An `https://` URL without quotes gets a
  hint. A name with a port without quotes gets the usage text.

Answers:

- The work runs in the background. The answer comes a moment after the command.
- It goes to the chat or console the command was given in. A command over RCON is answered in the server console.
- `clone` answers `<name>: cloned (main abc1234)`, `<name>: already there (main abc1234)` or `<name>: <error>`.

Example:

```
> sm_git list
github.com/owner/plugins   main 3f9c2ab   fetched 4 min ago
    open: updater.smx   watching: main (300 s)
> sm_git status plugins
github.com/owner/plugins
  url       https://github.com/owner/plugins
  head      main 3f9c2ab
  fetched   4 min ago
  open      updater.smx
  watching  main (300 s)
  branches  main 3f9c2ab, dev 81d07e4
  tags      v1.0 1a2b3c4
> sm_git log plugins
3f9c2ab 1 day ago, Jane Doe: Fix the spawn config
81d07e4 3 days ago, Jane Doe: Add the updater plugin
> sm_git fetch plugins
github.com/owner/plugins: fetched, nothing new
```

`-` stands for none, `(empty)` for a repository without commits, `never` for an unknown fetch time.

## Security

Plugins choose the remotes. These are the bounds the extension keeps regardless.

Network:

- Only `https://` remotes. The exact URL rules are in [reference.md](reference.md#remote-urls).
- Server certificates are checked against `configs/git/ca-bundle.crt`, plus `ca_file`.
- A redirect is followed only on the same host (another path or port). One to another host, or from `https://` to any
  other scheme, ends with `Git_Network`.
- The server's own git configuration (`~/.gitconfig`, `/etc/gitconfig`) is not read.
- Read-only towards the remote: nothing is ever pushed.

Disk:

- Write-outs stay inside `tf/`, never write into the root, never follow symlinks out. See
  [Write-out rules](reference.md#write-out-rules).
- Size limits bound clones, fetches and write-outs. See [Limits](reference.md#limits).

What the limits cannot bound: a hostile remote can still make the server use much memory. libgit2 keeps the remote's
list of branches and tags in memory without a bound. It also inflates each object a delta is based on whole, which may
be far larger than the bytes downloaded. Clone only remotes you trust that far.

## Sharing a root

Several servers may point `root` at one directory. Every server clones and fetches there. There are no roles.

How it stays consistent:

- A clone is made in a temp directory of its own, `<name>.git+tmp-XXXXXX` (a random name), and renamed into place.
  A repository in the root is always complete. Of two simultaneous clones, the first wins.
- Ref updates, and the end of a download (where libgit2 rewrites a locked file), wait for another process's lock.
- A watch compares the refs on disk with what it saw last, so it also tells what another server fetched.

Consequences:

- A server that cannot reach a remote cannot rely on another one to fetch for it. Its own fetches fail, though its
  watches still notice what the other server fetched.
- `sm_git remove` and `GitRepo.Remove` remove the repository for every server. Only the plugins of the removing server
  are asked whether they still use it.

## Crashes and leftovers

A server that crashes or is killed mid-work leaves nothing that blocks the others.

| Interrupted | What is left | Cleaned up |
|---|---|---|
| A removal | `<name>.git+removing-XXXXXX`. The repository was renamed there first, so it was gone for every reader at once. | When the extension loads, on any server sharing the root. |
| A clone | `<name>.git+tmp-XXXXXX` | At an extension load, if it has not grown for over an hour by then. A younger one waits for a later load. |
| A ref update or download | lock files in the repository | By the next ref update or download that waits five seconds for a lock in vain. It deletes the lock files older than ten minutes, in the repository's top directory and below `refs/`, and tries once more. |
| A write-out | `<file>.git-tmp` | Replaced by the next write-out that changes that file. Deleted with the file when a `WriteDir` with `oldRev` deletes it. |

- The cleanup at load runs on a worker thread, not the game thread.
- Each deletion is logged.
- An exception inside the extension's work ends that work with `Git_Failed` for its plugin, not the server.

## Unloading

- Unloading the extension aborts a running clone or fetch and waits for it to end.
- Queued work and finished results not yet handed out are dropped. No callbacks come.
- libgit2 lets the abort in before every connection, after every TLS handshake and whenever download data arrives. A
  ref update waiting for another process's lock stops waiting.
- While libgit2 waits for a remote to connect or to send its list of refs, it runs no code of the extension. Only
  `connect_timeout` and `stall_timeout` bound that wait. A remote that keeps sending a little within every
  `stall_timeout` holds the unload up for as long as it does so.
- An aborted clone deletes its own temp directory.
- A clone held up by a stalling remote keeps its temp directory. Once that has not grown for an hour, another server
  sharing the root deletes it when its extension loads. The held-up clone then fails.

## A repository only grows

libgit2 cannot repack or prune. So:

- objects made unreachable by force pushes, rebases or dropped tags stay;
- every fetch with news adds a pack.

Once a repository is close to `max_size_mb`, a fetch whose download would take it past the cap fails with
`Git_TooLarge`, also a watch's. Each failed fetch of a watch logs an error with the size.

The way out, either:

- remove it (`sm_git remove`, or `GitRepo.Remove` with no plugin using it) and clone it anew, which downloads only what
  is reachable;
- or raise `max_size_mb`.

## Logs

The extension logs to SourceMod's logs. Among its messages:

| Message | Kind |
|---|---|
| `Deleted the leftover <path>` | message |
| `Deleted the stale lock file <path> ...` | message |
| `Cleaning up the root: <problem>` | error |
| `Polling <name> failed: <reason>` | error |
| `Delivering a result failed: <reason>` | error |
| `a task ended with an exception and no answer: <reason>` | error |
| `a finished task was dropped: out of memory` | error |

/**
 * Copyright 2026 ETF2L Contributors, Dave
 *
 * Git: clones and fetches git repositories over HTTPS and reads them.
 *
 * A repository is named by its remote URL (https:// only) and kept as a bare
 * clone below the root of addons/sourcemod/configs/git.cfg. GitRepo.Open
 * gives a handle on it, cloning it first when it is not there yet. The
 * GitRepo handle belongs to the plugin, which deletes it when done; while it
 * is open, GitRepo.Remove of every other handle on the repository ends with
 * Git_InUse.
 *
 * Everything that does git work queues it; the extension runs it on a
 * thread of its own (one for clones, fetches and removals, one for
 * everything else, each in order) and calls the callback on a later frame.
 * Every callback gets a GitStatus first and an error text and the data last.
 * A result handle is only valid with Git_OK, except the GitChanges of
 * WriteFile and WriteDir, which is always valid; the plugin deletes every
 * valid one. Callbacks of a plugin that unloads before the work ends are not
 * called; the work still runs to its end.
 *
 * A revision ("rev") is anything git resolves to a commit: a branch, a tag,
 * a SHA, "HEAD", "main~2".
 *
 * Paths inside a repository are relative to its top, with "" for the top
 * itself; a trailing "/" is dropped. They may not start with "/" and may not
 * have empty ("a//b"), ".", ".." or ".git" parts (".git" in any case).
 * Paths on the server are relative to the game directory (tf/) and follow
 * the same rules, but are never empty and have no backslashes. A path that
 * breaks a rule raises a native error.
 *
 * Getters that fill a buffer return the bytes written into it. The getters
 * of result handles raise a native error for an invalid handle, a handle
 * of another kind, or an index outside 0 to Length - 1.
 *
 * Submodules and Git LFS are not followed, and symlinks are never written.
 * Nothing is written into the root or, through a symlink, outside the game
 * directory, and no file or directory larger than the repository size cap
 * of git.cfg. A remote is followed through redirects only on its own host.
 *
 * A repository only grows (nothing repacks it): close to the size cap, a
 * fetch whose download would take it past the cap, also a watch's, fails
 * with Git_TooLarge until it is removed and cloned anew, or the cap raised.
 */

#if defined _git_included
#endinput
#endif
#define _git_included

/**
 * How queued work ended.
 */
enum GitStatus {
	Git_OK = 0, /**< Success */
	Git_NotCloned, /**< The repository is not (or no longer) on disk */
	Git_NotFound, /**< No such revision, or no such path in the commit */
	Git_Refused, /**< A URL that is not accepted, a fetch of a repository whose stored remote URL is no longer accepted, a destination inside the root or leading out of the game directory, a write-out of a path with a file or directory named "*.git-tmp", or a repository with unsafe file names or directories nested deeper than 64 levels */
	Git_Network, /**< The remote could not be reached, answered wrongly or redirected to another host or scheme */
	Git_Timeout, /**< The remote took too long */
	Git_TooLarge, /**< Beyond the repository size cap, a remote with more than 10000 branches and tags, a file larger than maxBytes or than a write-out takes, a directory whose files together are, or a directory listing beyond 100000 entries */
	Git_InUse, /**< A plugin has the repository open, is opening it or watches it */
	Git_Failed /**< Anything else; the error text tells */
};

/** How a file differs between two revisions. */
enum GitDiffStatus {
	GitDiff_Added = 0,
	GitDiff_Modified,
	GitDiff_Deleted
};

/** What an entry of a directory is. */
enum GitEntryType {
	GitEntry_File = 0,
	GitEntry_Directory,
	GitEntry_Symlink,
	GitEntry_Submodule
};

/** What a write-out did to a file on the server. */
enum GitChangeKind {
	GitChange_Written = 0,
	GitChange_Deleted
};

/** How a file on the server compares to a file in a commit. */
enum GitFileState {
	GitFile_Same = 0, /**< Exactly the commit's content */
	GitFile_Different,
	GitFile_Missing /**< No such file on the server */
};

/**
 * The branches and tags of a repository at one moment.
 */
methodmap GitRefList < Handle {
	property int Length {
		public native get();
	}
	/** The short name: "main", "v1.0". */
	public native int GetName(int index, char[] buffer, int maxlength);
	/** The commit it points to. */
	public native int GetSha(int index, char[] buffer, int maxlength);
	public native bool IsTag(int index);
	/** The default branch; empty in an empty repository. */
	public native int GetHeadBranch(char[] buffer, int maxlength);
	public native int GetHeadSha(char[] buffer, int maxlength);
}

/**
 * Commits, newest first.
 */
methodmap GitCommitList < Handle {
	property int Length {
		public native get();
	}
	public native int GetSha(int index, char[] buffer, int maxlength);
	public native int GetAuthorName(int index, char[] buffer, int maxlength);
	public native int GetAuthorEmail(int index, char[] buffer, int maxlength);
	/** The author time as a Unix timestamp. */
	public native int GetTime(int index);
	/** The first line of the message. */
	public native int GetSummary(int index, char[] buffer, int maxlength);
	public native int GetMessage(int index, char[] buffer, int maxlength);
}

/**
 * The files that differ between two revisions, sorted by path. A renamed
 * file is a deleted and an added one.
 */
methodmap GitDiff < Handle {
	property int Length {
		public native get();
	}
	public native int GetPath(int index, char[] buffer, int maxlength);
	public native GitDiffStatus GetStatus(int index);
	/** The content's SHA in the base; empty for an added file. */
	public native int GetOldSha(int index, char[] buffer, int maxlength);
	/** The content's SHA in the head; empty for a deleted file. */
	public native int GetNewSha(int index, char[] buffer, int maxlength);
}

/**
 * The entries of a directory at a revision, each directory before its own
 * entries.
 */
methodmap GitTree < Handle {
	property int Length {
		public native get();
	}
	/** The path from the repository's top. */
	public native int GetPath(int index, char[] buffer, int maxlength);
	public native GitEntryType GetType(int index);
	/** The bytes of a file; 0 for directories and submodules. */
	public native int GetSize(int index);
	public native int GetSha(int index, char[] buffer, int maxlength);
	public native bool IsExecutable(int index);
}

/**
 * What a write-out changed on the server: deleted files first, then written
 * ones. Files that already had the commit's content are not in it.
 */
methodmap GitChanges < Handle {
	property int Length {
		public native get();
	}
	/** The file, relative to the game directory. */
	public native int GetPath(int index, char[] buffer, int maxlength);
	public native GitChangeKind GetKind(int index);
}

/**
 * The repositories below the root.
 */
methodmap GitRepoList < Handle {
	property int Length {
		public native get();
	}
	/** "<host>[:<port>]/<path>", e.g. "github.com/owner/name". */
	public native int GetName(int index, char[] buffer, int maxlength);
	public native int GetUrl(int index, char[] buffer, int maxlength);
	public native int GetHeadBranch(int index, char[] buffer, int maxlength);
	public native int GetHeadSha(int index, char[] buffer, int maxlength);
	/** When it was last fetched, by any server sharing the root, as a Unix timestamp; 0 when unknown. */
	public native int GetFetchTime(int index);
	/** The plugins of this server that have it open, e.g. "a.smx, b.smx"; empty for none. */
	public native int GetOpeners(int index, char[] buffer, int maxlength);
	/** The refs watched on this server, e.g. "main (300 s)"; empty for none. */
	public native int GetWatchers(int index, char[] buffer, int maxlength);
}

methodmap GitRepo < Handle {
	/**
	 * Opens the repository of a remote, cloning it first when it is not below
	 * the root yet. The clone has every branch and tag.
	 *
	 * @param url        https://<host>[:<port>]/<path>, with or without ".git";
	 *                   no other part of it may end in ".git", and the last
	 *                   may not be only ".git". Anything else ends with
	 *                   Git_Refused. An open called after a GitRepo.Remove
	 *                   of the repository passed its in-use check waits
	 *                   for the removal to end.
	 * @param callback   Gets the repository and whether it had to be cloned.
	 * @param data       Passed on to the callback.
	 * @param clone      With false, a repository that is not there is not
	 *                   cloned: the callback gets Git_NotCloned.
	 * @error            An invalid callback.
	 */
	public static native void Open(const char[] url, GitRepoCallback callback, any data = 0, bool clone = true);

	/**
	 * Fetches every branch and tag, drops those the remote no longer has and
	 * follows the remote's default branch. Watched refs that moved are told
	 * through Git_OnRefChanged before the callback.
	 *
	 * @param callback   Gets whether a branch or tag moved, appeared or went
	 *                   away, or the default branch changed; always false
	 *                   unless Git_OK.
	 * @error            An invalid handle or callback.
	 */
	public native void Fetch(GitFetchCallback callback, any data = 0);

	/**
	 * Resolves a revision to the SHA of its commit.
	 *
	 * @error            An invalid handle or callback, or an empty rev.
	 */
	public native void Resolve(const char[] rev, GitResolveCallback callback, any data = 0);

	/**
	 * Lists the branches and tags.
	 *
	 * @error            An invalid handle or callback.
	 */
	public native void ListRefs(GitRefsCallback callback, any data = 0);

	/**
	 * Lists the commits reachable from a revision, newest first.
	 *
	 * @param path       Only commits that changed this file or directory.
	 * @param max        At most this many commits, 1 to 1000.
	 * @param since      Only commits authored at or after this Unix
	 *                   timestamp. The history is read newest commit first
	 *                   and only until a commit committed more than a day
	 *                   before since, so a commit authored after since but
	 *                   committed earlier than that is not listed.
	 * @error            An invalid handle or callback, an empty rev, an
	 *                   invalid path or a max outside 1 to 1000.
	 */
	public native void Log(const char[] rev, GitLogCallback callback, any data = 0, const char[] path = "",
	                       int max = 50, int since = 0);

	/**
	 * Lists the files that differ between two revisions.
	 *
	 * @param path       Only files below this directory.
	 * @error            An invalid handle or callback, an empty base or head,
	 *                   or an invalid path.
	 */
	public native void Diff(const char[] base, const char[] head, GitDiffCallback callback, any data = 0,
	                        const char[] path = "");

	/**
	 * Lists the entries of a directory at a revision.
	 *
	 * @param path       The directory; Git_NotFound when the commit has none there.
	 * @param recursive  Also the entries of its directories. More than 100000
	 *                   entries end with Git_TooLarge; unsafe names or
	 *                   directories nested deeper than 64 levels below path
	 *                   with Git_Refused.
	 * @error            An invalid handle or callback, an empty rev or an
	 *                   invalid path.
	 */
	public native void ListTree(const char[] rev, GitTreeCallback callback, any data = 0, const char[] path = "",
	                            bool recursive = true);

	/**
	 * Reads a file at a revision into a string.
	 *
	 * The content is copied onto the plugin's heap for the callback, which by
	 * default has room for about 16 KB with the stack: a plugin that reads
	 * larger files needs a "#pragma dynamic" to match.
	 *
	 * @param maxBytes   A larger file ends with Git_TooLarge.
	 * @error            An invalid handle or callback, an empty rev, an
	 *                   invalid or empty path, or a maxBytes below 1.
	 */
	public native void ReadFile(const char[] rev, const char[] path, GitReadCallback callback, any data = 0,
	                            int maxBytes = 4096);

	/**
	 * Writes a file at a revision to the server, through a temp file renamed
	 * onto the destination, unless the destination already has that content.
	 * Missing directories are created. The temp file, "<dest>.git-tmp", is
	 * created anew, never through a symlink, so a dest with a file or
	 * directory whose name ends in ".git-tmp" ends with Git_Refused.
	 *
	 * @param path       The file in the repository.
	 * @param dest       The file on the server, relative to the game directory.
	 * @param callback   Gets the changes: one entry, or none when nothing had
	 *                   to be written or the write-out failed. The handle is
	 *                   valid whatever the status and must be deleted.
	 * @error            An invalid handle or callback, an empty rev, or an
	 *                   invalid or empty path or dest.
	 */
	public native void WriteFile(const char[] rev, const char[] path, const char[] dest,
	                             GitWriteCallback callback, any data = 0);

	/**
	 * Writes every file of a directory at a revision below a directory on the
	 * server, each like WriteFile. Symlinks and submodules are left out.
	 *
	 * With an oldRev, the files oldRev had in that directory and rev no longer
	 * has are deleted on the server first. Without one nothing is ever deleted,
	 * and files the repository never had are never touched.
	 *
	 * The files are written one by one; when one fails the callback gets the
	 * failure together with the files deleted and written before it, which
	 * stay so. Nothing is written when the files together are larger than
	 * the repository size cap, or the directory has more than 100000 entries
	 * (Git_TooLarge), or destDir or a path of rev or oldRev there has a file
	 * or directory whose name ends in ".git-tmp" (Git_Refused).
	 *
	 * @param path       The directory in the repository, "" for all of it.
	 * @param destDir    The directory on the server, relative to the game directory.
	 * @param callback   Gets the changes made. The handle is valid whatever
	 *                   the status and must be deleted.
	 * @param oldRev     The revision written there before, or "".
	 * @error            An invalid handle or callback, an empty rev, or an
	 *                   invalid path or destDir.
	 */
	public native void WriteDir(const char[] rev, const char[] path, const char[] destDir,
	                            GitWriteCallback callback, any data = 0, const char[] oldRev = "");

	/**
	 * Tells whether a file on the server has the content of a file at a
	 * revision. The file is checked like a write-out's dest (Git_Refused
	 * inside the root or, through a symlink, outside the game directory); a
	 * symlink or anything else but a regular file there is
	 * GitFile_Different, never followed or read.
	 *
	 * @param path       The file in the repository; Git_NotFound when the commit has none there.
	 * @param file       The file on the server, relative to the game directory.
	 * @param callback   Gets the state; GitFile_Different whenever the
	 *                   status is not Git_OK.
	 * @error            An invalid handle or callback, an empty rev, or an
	 *                   invalid or empty path or file.
	 */
	public native void CompareFile(const char[] rev, const char[] path, const char[] file,
	                               GitCompareCallback callback, any data = 0);

	/**
	 * Has the repository fetched on a timer, for as long as this plugin is
	 * loaded or until Unwatch: Git_OnRefChanged is called whenever the ref is
	 * at another commit than when it was last seen. A ref no plugin watched
	 * yet is fetched at once, which only notes where it is; a ref watched
	 * already keeps the repository's timer. Several plugins watching the
	 * same repository share its fetches, at the shortest interval any of
	 * them asked for. Watching a ref again changes this plugin's interval.
	 * The watch belongs to the plugin, not to this handle: deleting the
	 * handle does not end it, and it keeps GitRepo.Remove refused.
	 *
	 * @param ref        "HEAD" (the default branch) or the short name of a
	 *                   branch or tag ("main", "v1.0"), which need not exist
	 *                   yet. A name that is a branch and a tag follows the
	 *                   branch, while a revision of that name resolves to the
	 *                   tag, as in git: use the SHAs of Git_OnRefChanged.
	 * @param interval   Seconds between fetches, at least 60 (less is
	 *                   raised to 60); 0 for the watch_interval of git.cfg.
	 *                   It may be longer than watch_interval.
	 * @error            An invalid handle, a ref that is no such name
	 *                   ("refs/heads/main", "main~1", ""), or a negative
	 *                   interval.
	 */
	public native void Watch(const char[] ref, int interval = 0);

	/**
	 * Ends this plugin's watch of a ref; nothing happens when it has none.
	 *
	 * @error            An invalid handle.
	 */
	public native void Unwatch(const char[] ref);

	/**
	 * Deletes the repository from disk, for every server sharing the root.
	 * It waits for the work queued before it. Refused with Git_InUse, the
	 * error text naming the plugins, while a plugin watches it, has it open
	 * through another handle than this one, or is opening it (a
	 * GitRepo.Open that clones it or waits for an earlier removal). An open
	 * called after this removal passed that check waits for it to end.
	 * This handle stays valid; what it queues afterwards ends with
	 * Git_NotCloned.
	 *
	 * @error            An invalid handle or callback.
	 */
	public native void Remove(GitStatusCallback callback, any data = 0);

	/**
	 * The normalized remote URL, without ".git".
	 *
	 * @error            An invalid handle.
	 */
	public native int GetUrl(char[] buffer, int maxlength);

	/**
	 * "<host>[:<port>]/<path>", e.g. "github.com/owner/name".
	 *
	 * @error            An invalid handle.
	 */
	public native int GetName(char[] buffer, int maxlength);
}

typedef GitRepoCallback = function void (GitStatus status, GitRepo repo, bool cloned, const char[] error, any data);
typedef GitFetchCallback = function void (GitStatus status, bool changed, const char[] error, any data);
typedef GitResolveCallback = function void (GitStatus status, const char[] sha, const char[] error, any data);
typedef GitRefsCallback = function void (GitStatus status, GitRefList refs, const char[] error, any data);
typedef GitLogCallback = function void (GitStatus status, GitCommitList commits, const char[] error, any data);
typedef GitDiffCallback = function void (GitStatus status, GitDiff diff, const char[] error, any data);
typedef GitTreeCallback = function void (GitStatus status, GitTree tree, const char[] error, any data);
/** content holds length bytes; a binary file may have zero bytes inside. */
typedef GitReadCallback = function void (GitStatus status, const char[] content, int length, const char[] error,
                                         any data);
typedef GitWriteCallback = function void (GitStatus status, GitChanges changes, const char[] error, any data);
typedef GitCompareCallback = function void (GitStatus status, GitFileState state, const char[] error, any data);
typedef GitStatusCallback = function void (GitStatus status, const char[] error, any data);
typedef GitRepoListCallback = function void (GitStatus status, GitRepoList repos, const char[] error, any data);

/**
 * Lists the repositories below the root, whoever cloned them. Directories
 * below the root that cannot be read are left out; Git_Failed when the
 * root itself cannot be read.
 *
 * @error            An invalid callback.
 */
native void Git_ListRepositories(GitRepoListCallback callback, any data = 0);

/**
 * Called when a watched ref is at another commit than when it was last
 * seen, whether this server fetched it or another one sharing the root.
 * Every plugin that has this forward gets it, also for the watches of other
 * plugins: compare url and ref with the plugin's own.
 *
 * @param url        The repository's normalized remote URL, as GitRepo.GetUrl gives it.
 * @param ref        The ref as it was named in GitRepo.Watch.
 * @param oldSha     The commit it was at; empty when it did not exist.
 * @param newSha     The commit it is at; empty when it is gone.
 */
forward void Git_OnRefChanged(const char[] url, const char[] ref, const char[] oldSha, const char[] newSha);

/**
 * Do not edit below this line!
 */
public Extension __ext_git = {
	name = "Git",
	file = "git.ext",
#if defined AUTOLOAD_EXTENSIONS
	autoload = 1,
#else
	autoload = 0,
#endif
#if defined REQUIRE_EXTENSIONS
	required = 1,
#else
	required = 0,
#endif
};

#if !defined REQUIRE_EXTENSIONS
public void __ext_git_SetNTVOptional() {
	MarkNativeAsOptional("GitRepo.Open");
	MarkNativeAsOptional("GitRepo.Fetch");
	MarkNativeAsOptional("GitRepo.Resolve");
	MarkNativeAsOptional("GitRepo.ListRefs");
	MarkNativeAsOptional("GitRepo.Log");
	MarkNativeAsOptional("GitRepo.Diff");
	MarkNativeAsOptional("GitRepo.ListTree");
	MarkNativeAsOptional("GitRepo.ReadFile");
	MarkNativeAsOptional("GitRepo.WriteFile");
	MarkNativeAsOptional("GitRepo.WriteDir");
	MarkNativeAsOptional("GitRepo.CompareFile");
	MarkNativeAsOptional("GitRepo.Watch");
	MarkNativeAsOptional("GitRepo.Unwatch");
	MarkNativeAsOptional("GitRepo.Remove");
	MarkNativeAsOptional("GitRepo.GetUrl");
	MarkNativeAsOptional("GitRepo.GetName");
	MarkNativeAsOptional("Git_ListRepositories");
	MarkNativeAsOptional("GitRefList.Length.get");
	MarkNativeAsOptional("GitRefList.GetName");
	MarkNativeAsOptional("GitRefList.GetSha");
	MarkNativeAsOptional("GitRefList.IsTag");
	MarkNativeAsOptional("GitRefList.GetHeadBranch");
	MarkNativeAsOptional("GitRefList.GetHeadSha");
	MarkNativeAsOptional("GitCommitList.Length.get");
	MarkNativeAsOptional("GitCommitList.GetSha");
	MarkNativeAsOptional("GitCommitList.GetAuthorName");
	MarkNativeAsOptional("GitCommitList.GetAuthorEmail");
	MarkNativeAsOptional("GitCommitList.GetTime");
	MarkNativeAsOptional("GitCommitList.GetSummary");
	MarkNativeAsOptional("GitCommitList.GetMessage");
	MarkNativeAsOptional("GitDiff.Length.get");
	MarkNativeAsOptional("GitDiff.GetPath");
	MarkNativeAsOptional("GitDiff.GetStatus");
	MarkNativeAsOptional("GitDiff.GetOldSha");
	MarkNativeAsOptional("GitDiff.GetNewSha");
	MarkNativeAsOptional("GitTree.Length.get");
	MarkNativeAsOptional("GitTree.GetPath");
	MarkNativeAsOptional("GitTree.GetType");
	MarkNativeAsOptional("GitTree.GetSize");
	MarkNativeAsOptional("GitTree.GetSha");
	MarkNativeAsOptional("GitTree.IsExecutable");
	MarkNativeAsOptional("GitChanges.Length.get");
	MarkNativeAsOptional("GitChanges.GetPath");
	MarkNativeAsOptional("GitChanges.GetKind");
	MarkNativeAsOptional("GitRepoList.Length.get");
	MarkNativeAsOptional("GitRepoList.GetName");
	MarkNativeAsOptional("GitRepoList.GetUrl");
	MarkNativeAsOptional("GitRepoList.GetHeadBranch");
	MarkNativeAsOptional("GitRepoList.GetHeadSha");
	MarkNativeAsOptional("GitRepoList.GetFetchTime");
	MarkNativeAsOptional("GitRepoList.GetOpeners");
	MarkNativeAsOptional("GitRepoList.GetWatchers");
}
#endif

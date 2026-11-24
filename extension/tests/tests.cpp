/* Copyright 2026 ETF2L Contributors, Dave
 *
 * Unit tests of the core. They make their repositories with the git command
 * in a fresh directory below /tmp and exit non-zero when a check failed.
 * Cloning and fetching need an HTTPS server and are tested end to end on
 * the test server; the watches' schedule runs on a time the tests set.
 *
 * Usage: git-tests-<arch> <CA bundle> */
#include "config.h"
#include "remote.h"
#include "repo.h"
#include "root.h"
#include "serverlog.h"
#include "watches.h"
#include "worker.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace smgit;

namespace {

std::string g_dir;
std::string g_repo;
int g_failures = 0;

/* Write-outs in the tests may go anywhere below the test directory but into
 * the root, and no larger than maxBytes. */
WritePolicy Policy(uint64_t maxBytes = 1024 * 1024)
{
	return WritePolicy{CanonicalPath(g_dir).value, CanonicalPath(g_dir + "/root").value, maxBytes};
}

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                     \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
			g_failures++;                                                       \
		}                                                                       \
	} while (0)

#define CHECK_EQUAL(actual, expected)                                           \
	do {                                                                        \
		std::string a = (actual), e = (expected);                               \
		if (a != e) {                                                           \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, e.c_str(), a.c_str()); \
			g_failures++;                                                       \
		}                                                                       \
	} while (0)

std::string ReadLocal(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteLocal(const std::string &path, const std::string &content)
{
	std::ofstream(path, std::ios::binary) << content;
}

bool ExistsLocal(const std::string &path)
{
	return access(path.c_str(), F_OK) == 0;
}

/* The names in a directory. */
std::vector<std::string> NamesIn(const std::string &directory)
{
	std::vector<std::string> names;
	std::error_code failure;
	for (std::filesystem::directory_iterator it(directory, failure), end; !failure && it != end; it.increment(failure))
		names.push_back(it->path().filename().string());
	return names;
}

bool Contains(const std::string &text, const std::string &part)
{
	return text.find(part) != std::string::npos;
}

void Shell(const std::string &script)
{
	std::string command = "set -e; cd '" + g_dir + "'; " + script;
	if (system(command.c_str()) != 0) {
		fprintf(stderr, "fixture command failed: %s\n", script.c_str());
		exit(2);
	}
}

/* Shell functions for crafted trees, which git itself would never write:
 * "entry <mode> <name> <sha>" prints one tree entry, "maketree" makes a tree of
 * the entries on its input and prints its SHA, "publish <branch> <tree>"
 * commits it to the branch and pushes that into the bare copy. Run in
 * work/. */
const char kTreeTools[] =
	"cd work && blob=$(git rev-parse v1:plugins/a.txt)"
	" && entry() { printf '%s %s\\0' \"$1\" \"$2\"; perl -e 'print pack(\"H*\", shift)' \"$3\"; }"
	" && maketree() { git hash-object --literally -t tree -w --stdin; }"
	" && publish() { git update-ref \"refs/heads/$1\" \"$(git commit-tree -m \"$1\" \"$2\")\""
	"    && git push -q ../root/example.com/team/repo.git \"$1\"; }";

/* A work tree with two commits and a tag, and its bare copy below the root
 * where the extension would keep https://example.com/team/repo:
 *   first  (v1):  plugins/a.txt "one", cfg/server.cfg, run.sh (executable), link -> plugins/a.txt
 *   second (main): plugins/a.txt changed, plugins/b.txt added, cfg/server.cfg deleted */
void MakeFixture()
{
	Shell("git init -q -b main work && cd work"
	      " && git config user.name Tester && git config user.email tester@example.com"
	      " && mkdir plugins cfg && echo one > plugins/a.txt && echo cfg1 > cfg/server.cfg"
	      " && printf '#!/bin/sh\\n' > run.sh && chmod +x run.sh && ln -s plugins/a.txt link"
	      " && git add -A"
	      " && GIT_AUTHOR_DATE=2026-01-01T00:00:00Z GIT_COMMITTER_DATE=2026-01-01T00:00:00Z git commit -q -m first"
	      " && git tag v1"
	      " && echo changed > plugins/a.txt && echo two > plugins/b.txt && git rm -q cfg/server.cfg && git add -A"
	      " && GIT_AUTHOR_DATE=2026-02-01T00:00:00Z GIT_COMMITTER_DATE=2026-02-01T00:00:00Z"
	      "    git commit -q -m 'second' -m 'with a body'"
	      " && mkdir -p ../root/example.com/team"
	      " && git clone -q --bare . ../root/example.com/team/repo.git");
	g_repo = g_dir + "/root/example.com/team/repo.git";
}

void TestRemoteUrls()
{
	std::string error;
	std::optional<Remote> remote = ParseRemoteUrl("https://GitHub.com/Owner/Name.git/", error);
	CHECK(remote.has_value());
	CHECK_EQUAL(remote->url, "https://github.com/Owner/Name");
	CHECK_EQUAL(remote->name, "github.com/Owner/Name");
	CHECK_EQUAL(RepositoryPath("/root", *remote), "/root/github.com/Owner/Name.git");
	CHECK(ParseRemoteUrl("https://localhost:8443/test", error).has_value());

	for (const char *refused : {"http://github.com/a/b", "ssh://git@github.com/a/b", "git://github.com/a/b",
	                            "/srv/git/a", "file:///srv/git/a", "https://user@github.com/a/b",
	                            "https://github.com/a/../b", "https://github.com//b", "https://github.com/",
	                            "https://github.com", "https://github.com/a/b?x=1", "https://github.com:port/a",
	                            "https://github.com/a.git/b", "https://host.git/a", "https://github.com/owner/.git",
	                            "https://github.com/owner/.git/", "https://github.com/.git"})
		CHECK(!ParseRemoteUrl(refused, error).has_value());
	CHECK(!error.empty());
}

void TestRepoPaths()
{
	CHECK_EQUAL(*ParseRepoPath(""), "");
	CHECK_EQUAL(*ParseRepoPath("addons/sourcemod/"), "addons/sourcemod");
	for (const char *refused : {"/etc/passwd", "a//b", "../a", "a/../b", "a/.", "./a", "a/.git/config"})
		CHECK(!ParseRepoPath(refused).has_value());
}

/* Game paths: SourceMod would turn "..\" into "../", so backslashes are
 * refused along with everything a repository path may not have. */
void TestGamePaths()
{
	CHECK_EQUAL(*ParseGamePath("cfg/server.cfg"), "cfg/server.cfg");
	CHECK_EQUAL(*ParseGamePath("cfg/"), "cfg");
	for (const char *refused : {"", ".", "/", "/etc/passwd", "a//b", "a/./b", "../a", "a/../b", "..\\..\\x",
	                            "cfg\\server.cfg", "cfg/.git/config", "cfg/.GIT"})
		CHECK(!ParseGamePath(refused).has_value());

	CHECK_EQUAL(*PathBelow("/game", "cfg/a.cfg"), "/game/cfg/a.cfg");
	CHECK(!PathBelow("/game", "../x").has_value());
	CHECK(!PathBelow("/game", "a/../../x").has_value());
	CHECK(IsAtOrBelow("/game", "/game") && IsAtOrBelow("/game/./a", "/game/") && IsAtOrBelow("/a", "/"));
	CHECK(!IsAtOrBelow("/gamex", "/game") && !IsAtOrBelow("/game/../x", "/game"));
}

void TestEntryNames()
{
	CHECK(IsSafeEntryName("a.txt") && IsSafeEntryName(".gitignore") && IsSafeEntryName("a.git") &&
	      IsSafeEntryName("..\\x"));
	for (const char *refused : {"", ".", "..", ".git", ".GIT", ".Git", "a/b"})
		CHECK(!IsSafeEntryName(refused));
}

/* Watches follow HEAD and names a branch or tag can have; anything that
 * could never match one of them is refused. */
void TestWatchableRefs()
{
	for (const char *ref : {"HEAD", "main", "v1.0", "release/1.x", "feature-2"})
		CHECK(IsWatchableRef(ref));
	for (const char *ref : {"", "refs/heads/main", "refs/tags/v1", "main~1", "main^", "a..b", "x:y", "a b",
	                        "*", "main.lock", "/main", "main/", "a\\b"})
		CHECK(!IsWatchableRef(ref));
}

void TestConfig()
{
	std::string error;
	std::optional<Config> defaults = ParseConfig({}, error);
	CHECK(defaults && defaults->watchInterval == 300 && defaults->connectTimeout == 10 &&
	      defaults->stallTimeout == 30 && defaults->maxRepositoryBytes == 200ull * 1024 * 1024);

	std::optional<Config> set = ParseConfig({{"root", "/srv/git"}, {"ca_file", "ca.pem"}, {"watch_interval", "30"},
	                                         {"connect_timeout", "5"}, {"stall_timeout", "7"}, {"max_size_mb", "1"}},
	                                        error);
	CHECK(set && set->root == "/srv/git" && set->caFile == "ca.pem" && set->watchInterval == kMinWatchInterval &&
	      set->connectTimeout == 5 && set->stallTimeout == 7 && set->maxRepositoryBytes == 1024 * 1024);

	CHECK(!ParseConfig({{"stall_timeout", "soon"}}, error).has_value());
	CHECK(!ParseConfig({{"max_size_mb", "0"}}, error).has_value());
	CHECK(!ParseConfig({{"rooot", "/srv/git"}}, error).has_value());

	/* libgit2 takes the timeouts in milliseconds as an int. */
	CHECK(ParseConfig({{"connect_timeout", "3600"}, {"stall_timeout", "3600"}}, error).has_value());
	CHECK(!ParseConfig({{"connect_timeout", "3601"}}, error).has_value());
	CHECK(Contains(error, "at most 3600"));
	CHECK(!ParseConfig({{"stall_timeout", "999999999"}}, error).has_value());
}

void TestResolveAndRefs()
{
	Result<std::string> head = Resolve(g_repo, "HEAD");
	Result<std::string> first = Resolve(g_repo, "v1");
	CHECK(head.ok() && head.value.size() == 40);
	CHECK(first.ok() && first.value != head.value);
	CHECK_EQUAL(Resolve(g_repo, "main~1").value, first.value);
	CHECK(Resolve(g_repo, "nothing").status == Status::NotFound);
	CHECK(Resolve(g_dir + "/root/example.com/team/other.git", "HEAD").status == Status::NotCloned);

	Result<RefSnapshot> refs = ListRefs(g_repo);
	CHECK(refs.ok() && refs.value.refs.size() == 2);
	CHECK_EQUAL(refs.value.headBranch, "main");
	CHECK_EQUAL(refs.value.headSha, head.value);
	for (const Ref &ref : refs.value.refs) {
		CHECK_EQUAL(ref.name, ref.isTag ? "v1" : "main");
		CHECK_EQUAL(ref.sha, ref.isTag ? first.value : head.value);
	}
}

void TestLog()
{
	Result<std::vector<Commit>> all = Log(g_repo, "main", "", 50, 0);
	CHECK(all.ok() && all.value.size() == 2);
	CHECK_EQUAL(all.value[0].summary, "second");
	CHECK_EQUAL(all.value[0].message, "second\n\nwith a body\n");
	CHECK_EQUAL(all.value[0].authorName, "Tester");
	CHECK_EQUAL(all.value[0].authorEmail, "tester@example.com");
	CHECK(all.value[0].time == 1769904000);
	CHECK_EQUAL(all.value[1].summary, "first");

	CHECK(Log(g_repo, "main", "", 1, 0).value.size() == 1);
	CHECK(Log(g_repo, "main", "", 50, 1769904000).value.size() == 1);
	CHECK(Log(g_repo, "main", "plugins/b.txt", 50, 0).value.size() == 1);
	CHECK(Log(g_repo, "main", "plugins/a.txt", 50, 0).value.size() == 2);
	Result<std::vector<Commit>> untouched = Log(g_repo, "main", "run.sh", 50, 0);
	CHECK(untouched.value.size() == 1);
	CHECK_EQUAL(untouched.value[0].summary, "first");

	/* With since the walk ends at the first commit committed more than a
	 * day before it: "ancient" was authored after since, but committed in
	 * 2020, so it is not reached. "skewed" was committed half a day before
	 * since and is. since is 2026-04-01. */
	Shell("cd work && tree=$(git rev-parse 'main^{tree}')"
	      " && ancient=$(GIT_AUTHOR_DATE=2026-05-01T00:00:00Z GIT_COMMITTER_DATE=2020-01-01T00:00:00Z"
	      "    git commit-tree $tree -p main -m ancient)"
	      " && skewed=$(GIT_AUTHOR_DATE=2026-04-01T06:00:00Z GIT_COMMITTER_DATE=2026-03-31T12:00:00Z"
	      "    git commit-tree $tree -p $ancient -m skewed)"
	      " && newest=$(GIT_AUTHOR_DATE=2026-04-02T00:00:00Z GIT_COMMITTER_DATE=2026-04-02T00:00:00Z"
	      "    git commit-tree $tree -p $skewed -m newest)"
	      " && git update-ref refs/heads/history $newest"
	      " && git push -q ../root/example.com/team/repo.git history");
	Result<std::vector<Commit>> recent = Log(g_repo, "history", "", 50, 1775001600);
	CHECK(recent.ok() && recent.value.size() == 2);
	CHECK_EQUAL(recent.value[0].summary, "newest");
	CHECK_EQUAL(recent.value[1].summary, "skewed");
	CHECK(Log(g_repo, "history", "", 50, 0).value.size() == 5);
}

void TestDiff()
{
	Result<std::vector<DiffEntry>> diff = Diff(g_repo, "v1", "main", "");
	CHECK(diff.ok() && diff.value.size() == 3);
	CHECK_EQUAL(diff.value[0].path, "cfg/server.cfg");
	CHECK(diff.value[0].status == DiffStatus::Deleted && diff.value[0].newSha.empty() &&
	      diff.value[0].oldSha.size() == 40);
	CHECK_EQUAL(diff.value[1].path, "plugins/a.txt");
	CHECK(diff.value[1].status == DiffStatus::Modified && diff.value[1].oldSha != diff.value[1].newSha);
	CHECK_EQUAL(diff.value[2].path, "plugins/b.txt");
	CHECK(diff.value[2].status == DiffStatus::Added && diff.value[2].oldSha.empty());

	CHECK(Diff(g_repo, "v1", "main", "plugins").value.size() == 2);
	CHECK(Diff(g_repo, "main", "main", "").value.empty());
	CHECK(Diff(g_repo, "v1", "nothing", "").status == Status::NotFound);
}

void TestTree()
{
	Result<std::vector<TreeEntry>> top = ListTree(g_repo, "v1", "", false);
	CHECK(top.ok() && top.value.size() == 4);
	CHECK_EQUAL(top.value[0].path, "cfg");
	CHECK(top.value[0].type == EntryType::Directory);
	CHECK_EQUAL(top.value[1].path, "link");
	CHECK(top.value[1].type == EntryType::Symlink);
	CHECK_EQUAL(top.value[3].path, "run.sh");
	CHECK(top.value[3].type == EntryType::File && top.value[3].isExecutable && top.value[3].size == 10);

	Result<std::vector<TreeEntry>> all = ListTree(g_repo, "v1", "", true);
	CHECK(all.value.size() == 6);
	Result<std::vector<TreeEntry>> plugins = ListTree(g_repo, "main", "plugins", true);
	CHECK(plugins.value.size() == 2);
	CHECK_EQUAL(plugins.value[1].path, "plugins/b.txt");
	CHECK(plugins.value[1].size == 4 && !plugins.value[1].isExecutable);
	CHECK(ListTree(g_repo, "main", "cfg", true).status == Status::NotFound);
	CHECK(ListTree(g_repo, "main", "run.sh", true).status == Status::NotFound);
}

void TestReadFile()
{
	CHECK_EQUAL(ReadFile(g_repo, "v1", "plugins/a.txt", 100).value, "one\n");
	CHECK_EQUAL(ReadFile(g_repo, "main", "plugins/a.txt", 8).value, "changed\n");
	CHECK(ReadFile(g_repo, "main", "plugins/a.txt", 7).status == Status::TooLarge);
	CHECK(ReadFile(g_repo, "main", "cfg/server.cfg", 100).status == Status::NotFound);
	CHECK(ReadFile(g_repo, "main", "plugins", 100).status == Status::NotFound);
	CHECK(ReadFile(g_repo, "v1", "link", 100).status == Status::NotFound);
}

void TestWriteFile()
{
	Destination dest{g_dir + "/out/file/deep/a.txt", "file/deep/a.txt"};
	Result<std::vector<Change>> written = WriteFile(g_repo, "v1", "plugins/a.txt", dest, Policy());
	CHECK(written.ok() && written.value.size() == 1);
	CHECK_EQUAL(written.value[0].path, "file/deep/a.txt");
	CHECK(written.value[0].kind == ChangeKind::Written);
	CHECK_EQUAL(ReadLocal(dest.absolute), "one\n");
	CHECK(!ExistsLocal(dest.absolute + ".git-tmp"));

	CHECK(WriteFile(g_repo, "v1", "plugins/a.txt", dest, Policy()).value.empty());
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", dest, Policy()).value.size() == 1);
	CHECK_EQUAL(ReadLocal(dest.absolute), "changed\n");

	Destination script{g_dir + "/out/file/run.sh", "file/run.sh"};
	CHECK(WriteFile(g_repo, "main", "run.sh", script, Policy()).ok());
	CHECK(access(script.absolute.c_str(), X_OK) == 0);
	CHECK(WriteFile(g_repo, "main", "plugins", dest, Policy()).status == Status::NotFound);

	/* Not into the root, where it could change a repository, and not beyond
	 * the size a write-out takes. */
	Destination config{g_repo + "/config", "root/example.com/team/repo.git/config"};
	std::string before = ReadLocal(config.absolute);
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", config, Policy()).status == Status::Refused);
	CHECK_EQUAL(ReadLocal(config.absolute), before);
	Destination small{g_dir + "/out/file/small.txt", "file/small.txt"};
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", small, Policy(7)).status == Status::TooLarge);
	CHECK(!ExistsLocal(small.absolute));
	CHECK(WriteFile(g_repo, "v1", "link", dest, Policy()).status == Status::NotFound);
}

void TestWriteDir()
{
	Destination dest{g_dir + "/out/dir", "dir"};
	Result<std::vector<Change>> first = WriteDir(g_repo, "v1", "", dest, "", Policy());
	CHECK(first.ok() && first.value.size() == 3);
	CHECK_EQUAL(ReadLocal(dest.absolute + "/cfg/server.cfg"), "cfg1\n");
	CHECK(!ExistsLocal(dest.absolute + "/link"));
	CHECK(WriteDir(g_repo, "v1", "", dest, "", Policy()).value.empty());

	/* A file the repository never had, and one it had that was edited. */
	WriteLocal(dest.absolute + "/cfg/own.cfg", "mine");
	WriteLocal(dest.absolute + "/run.sh", "edited");

	Result<std::vector<Change>> second = WriteDir(g_repo, "main", "", dest, "v1", Policy());
	CHECK(second.ok() && second.value.size() == 4);
	CHECK_EQUAL(second.value[0].path, "dir/cfg/server.cfg");
	CHECK(second.value[0].kind == ChangeKind::Deleted);
	CHECK_EQUAL(second.value[1].path, "dir/plugins/a.txt");
	CHECK_EQUAL(second.value[2].path, "dir/plugins/b.txt");
	CHECK_EQUAL(second.value[3].path, "dir/run.sh");
	CHECK(!ExistsLocal(dest.absolute + "/cfg/server.cfg"));
	CHECK_EQUAL(ReadLocal(dest.absolute + "/cfg/own.cfg"), "mine");
	CHECK_EQUAL(ReadLocal(dest.absolute + "/run.sh"), "#!/bin/sh\n");

	/* Without the old revision nothing is deleted. */
	Destination kept{g_dir + "/out/kept", "kept"};
	CHECK(WriteDir(g_repo, "v1", "", kept, "", Policy()).ok());
	CHECK(WriteDir(g_repo, "main", "", kept, "", Policy()).ok());
	CHECK(ExistsLocal(kept.absolute + "/cfg/server.cfg"));

	Destination part{g_dir + "/out/part", "part"};
	Result<std::vector<Change>> plugins = WriteDir(g_repo, "main", "plugins", part, "", Policy());
	CHECK(plugins.value.size() == 2);
	CHECK_EQUAL(plugins.value[0].path, "part/a.txt");
	CHECK(WriteDir(g_repo, "main", "cfg", part, "", Policy()).status == Status::NotFound);

	/* Every file fits the cap on its own (8, 4 and 10 bytes), but not
	 * together. */
	Destination capped{g_dir + "/out/capped", "capped"};
	CHECK(WriteDir(g_repo, "main", "", capped, "", Policy(12)).status == Status::TooLarge);
	CHECK(!ExistsLocal(capped.absolute));

	/* A write-out that fails partway still tells what it wrote: run.sh,
	 * written last, cannot replace a directory with a file in it. */
	Shell("mkdir -p out/partial/run.sh && touch out/partial/run.sh/blocker");
	Destination partial{g_dir + "/out/partial", "partial"};
	Result<std::vector<Change>> stopped = WriteDir(g_repo, "v1", "", partial, "", Policy());
	CHECK(stopped.status == Status::Failed && stopped.value.size() == 2);
	CHECK_EQUAL(stopped.value[0].path, "partial/cfg/server.cfg");
	CHECK_EQUAL(stopped.value[1].path, "partial/plugins/a.txt");
	Destination blocked{g_dir + "/out/partial/run.sh", "partial/run.sh"};
	Result<std::vector<Change>> single = WriteFile(g_repo, "v1", "run.sh", blocked, Policy());
	CHECK(single.status == Status::Failed && single.value.empty());
}

/* A destination is where its directory leads, symlinks and all: neither
 * into the root nor out of the game directory (out/ here). */
void TestDestinationSymlinks()
{
	Shell("mkdir -p out/links elsewhere && ln -s ../../root out/links/root && ln -s ../../elsewhere out/links/out");
	WritePolicy policy{CanonicalPath(g_dir + "/out").value, CanonicalPath(g_dir + "/root").value, 1024};
	Destination intoRoot{g_dir + "/out/links/root/planted.txt", "links/root/planted.txt"};
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", intoRoot, policy).status == Status::Refused);
	CHECK(!ExistsLocal(g_dir + "/root/planted.txt"));
	Destination outside{g_dir + "/out/links/out/planted.txt", "links/out/planted.txt"};
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", outside, policy).status == Status::Refused);
	CHECK(!ExistsLocal(g_dir + "/elsewhere/planted.txt"));
	Destination inside{g_dir + "/out/links/inside.txt", "links/inside.txt"};
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", inside, policy).ok());
}

/* A failed write tells why it failed, not what a later call said. The
 * process may write files of at most 4 bytes meanwhile, so writing the 8 of
 * "changed\n" fails with EFBIG; SIGXFSZ, which would end the process, is
 * ignored. */
void TestWriteErrors()
{
	struct rlimit before;
	CHECK(getrlimit(RLIMIT_FSIZE, &before) == 0);
	struct rlimit small = before;
	small.rlim_cur = 4;
	void (*oldHandler)(int) = signal(SIGXFSZ, SIG_IGN);
	CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
	Result<std::vector<Change>> full =
		WriteFile(g_repo, "main", "plugins/a.txt", Destination{g_dir + "/out/full/a.txt", "full/a.txt"}, Policy());
	CHECK(setrlimit(RLIMIT_FSIZE, &before) == 0);
	signal(SIGXFSZ, oldHandler);

	CHECK(full.status == Status::Failed && full.value.empty());
	CHECK(Contains(full.error, "File too large"));
	CHECK(!ExistsLocal(g_dir + "/out/full/a.txt") && !ExistsLocal(g_dir + "/out/full/a.txt.git-tmp"));
}

/* A symlink planted at a write-out's temp name is replaced, not written
 * through: the file it leads to keeps its content and mode. */
void TestWriteTempSymlink()
{
	Shell("mkdir -p out/planted && echo secret > planted-target && chmod 600 planted-target"
	      " && ln -s ../../planted-target out/planted/a.txt.git-tmp");
	Destination dest{g_dir + "/out/planted/a.txt", "planted/a.txt"};
	Result<std::vector<Change>> written = WriteFile(g_repo, "main", "plugins/a.txt", dest, Policy());
	CHECK(written.ok() && written.value.size() == 1);
	CHECK_EQUAL(ReadLocal(dest.absolute), "changed\n");
	CHECK_EQUAL(ReadLocal(g_dir + "/planted-target"), "secret\n");
	struct stat target;
	CHECK(stat((g_dir + "/planted-target").c_str(), &target) == 0 && (target.st_mode & 0777) == 0600);
	struct stat file;
	CHECK(lstat(dest.absolute.c_str(), &file) == 0 && S_ISREG(file.st_mode) && (file.st_mode & 0777) == 0644);
	CHECK(!ExistsLocal(dest.absolute + ".git-tmp"));
}

/* A file named like the write-out's temp files, in the repository or as a
 * destination, would be replaced by the temp file of its sibling: nothing
 * is written. */
void TestWriteTempNames()
{
	Shell(std::string(kTreeTools) +
	      " && publish tempname \"$({ entry 100644 a $blob; entry 100644 a.git-tmp $blob; } | maketree)\""
	      " && inner=$(entry 100644 b $blob | maketree)"
	      " && publish tempdir \"$({ entry 100644 a $blob; entry 40000 a.git-tmp $inner; } | maketree)\"");
	Destination dest{g_dir + "/out/tempname", "tempname"};
	for (const char *branch : {"tempname", "tempdir"}) {
		Result<std::vector<Change>> refused = WriteDir(g_repo, branch, "", dest, "", Policy());
		CHECK(refused.status == Status::Refused && refused.value.empty() && Contains(refused.error, ".git-tmp"));
		CHECK(!ExistsLocal(dest.absolute));
	}
	Destination intoTempDir{g_dir + "/out/x.git-tmp/dir", "x.git-tmp/dir"};
	CHECK(WriteDir(g_repo, "main", "plugins", intoTempDir, "", Policy()).status == Status::Refused);
	CHECK(!ExistsLocal(g_dir + "/out/x.git-tmp"));
	Destination named{g_dir + "/out/tempname/b.git-tmp", "tempname/b.git-tmp"};
	CHECK(WriteFile(g_repo, "main", "plugins/a.txt", named, Policy()).status == Status::Refused);
	CHECK(!ExistsLocal(named.absolute));
}

/* Commits whose trees have entries named "..", "." and ".git" (in two
 * spellings), which git itself would never write. Written out below
 * out/evil/inner, ".." would land on out/evil, "." on out/evil/inner and
 * ".git" would create out/evil/inner/.git. */
void TestUnsafeEntryNames()
{
	Shell(std::string(kTreeTools) +
	      " && publish dotdot \"$(entry 100644 .. $blob | maketree)\""
	      " && publish dot \"$(entry 100644 . $blob | maketree)\""
	      " && config=$(entry 100644 config $blob | maketree)"
	      " && publish dotgit \"$(entry 40000 .git $config | maketree)\""
	      " && publish dotGIT \"$(entry 40000 .GIT $config | maketree)\"");
	Destination dest{g_dir + "/out/evil/inner", "evil/inner"};
	for (const char *branch : {"dotdot", "dot", "dotgit", "dotGIT"}) {
		CHECK(WriteDir(g_repo, branch, "", dest, "", Policy()).status == Status::Refused);
		CHECK(ListTree(g_repo, branch, "", false).status == Status::Refused);
	}
	CHECK(!ExistsLocal(g_dir + "/out/evil"));
}

/* Directories nested deeper than the limit, and a tree that names 2^20
 * paths with 21 tree objects, each naming the one below it twice. */
void TestTreeBounds()
{
	Shell(std::string(kTreeTools) +
	      " && t=$(entry 100644 f $blob | maketree)"
	      " && i=0 && while [ $i -lt 70 ]; do t=$(entry 40000 d $t | maketree); i=$((i+1)); done"
	      " && publish deep $t"
	      " && t=$(entry 100644 f $blob | maketree)"
	      " && i=0 && while [ $i -lt 20 ]; do t=$({ entry 40000 a $t; entry 40000 b $t; } | maketree); i=$((i+1)); done"
	      " && publish dag $t");
	CHECK(ListTree(g_repo, "deep", "", true).status == Status::Refused);
	CHECK(ListTree(g_repo, "deep", "", false).ok());

	Result<std::vector<TreeEntry>> dag = ListTree(g_repo, "dag", "", true);
	CHECK(dag.status == Status::TooLarge);
	CHECK(Contains(dag.error, std::to_string(kMaxTreeEntries)));
	CHECK(ListTree(g_repo, "dag", "", false).value.size() == 2);
	Destination dest{g_dir + "/out/dag", "dag"};
	CHECK(WriteDir(g_repo, "dag", "", dest, "", Policy()).status == Status::TooLarge);
	CHECK(!ExistsLocal(dest.absolute));
}

void TestCompareFile()
{
	Destination file{g_dir + "/out/compare.txt", "compare.txt"};
	CHECK(CompareFile(g_repo, "v1", "plugins/a.txt", file, Policy()).value == FileState::Missing);
	WriteLocal(file.absolute, "one\n");
	CHECK(CompareFile(g_repo, "v1", "plugins/a.txt", file, Policy()).value == FileState::Same);
	CHECK(CompareFile(g_repo, "main", "plugins/a.txt", file, Policy()).value == FileState::Different);
	CHECK(CompareFile(g_repo, "main", "cfg/server.cfg", file, Policy()).status == Status::NotFound);
	WriteLocal(file.absolute, "one\nmore");
	CHECK(CompareFile(g_repo, "v1", "plugins/a.txt", file, Policy()).value == FileState::Different);
}

/* A file compared is where a write-out of it would go: not through a
 * symlinked directory out of the game directory (out/ here), and a symlink
 * or a FIFO at the file itself is other content, neither followed nor
 * waited on. */
void TestCompareFileLinks()
{
	Shell("mkdir -p out/cmp elsewhere-cmp && echo one > elsewhere-cmp/a.txt && ln -s ../../elsewhere-cmp out/cmp/out"
	      " && ln -s ../../elsewhere-cmp/a.txt out/cmp/link.txt && mkfifo out/cmp/fifo");
	WritePolicy policy{CanonicalPath(g_dir + "/out").value, CanonicalPath(g_dir + "/root").value, 1024};
	Destination outside{g_dir + "/out/cmp/out/a.txt", "cmp/out/a.txt"};
	CHECK(CompareFile(g_repo, "v1", "plugins/a.txt", outside, policy).status == Status::Refused);
	Destination link{g_dir + "/out/cmp/link.txt", "cmp/link.txt"};
	Result<FileState> linked = CompareFile(g_repo, "v1", "plugins/a.txt", link, policy);
	CHECK(linked.ok() && linked.value == FileState::Different);
	Destination fifo{g_dir + "/out/cmp/fifo", "cmp/fifo"};
	Result<FileState> piped = CompareFile(g_repo, "v1", "plugins/a.txt", fifo, policy);
	CHECK(piped.ok() && piped.value == FileState::Different);
}

void TestListAndRemove()
{
	Shell("git clone -q --bare work root/example.com/second.git && mkdir -p root/example.com/empty.git");
	std::string root = g_dir + "/root";
	Result<std::vector<RepositoryInfo>> listed = ListRepositories(root);
	CHECK(listed.ok() && listed.value.size() == 2);
	bool hasRepo = false;
	for (const RepositoryInfo &info : listed.value) {
		if (info.name != "example.com/team/repo")
			continue;
		hasRepo = true;
		CHECK_EQUAL(info.headBranch, "main");
		CHECK_EQUAL(info.headSha, Resolve(g_repo, "main").value);
	}
	CHECK(hasRepo);
	CHECK(ListRepositories(g_dir + "/no-root").value.empty());

	std::string second = root + "/example.com/second.git";
	CHECK(Exists(second));
	CHECK(Remove(second).ok());
	CHECK(!ExistsLocal(second));
	CHECK(Remove(second).status == Status::NotCloned);
	CHECK(ListRepositories(root).value.size() == 1);
	/* Nothing is left of the directory the repository was moved into. */
	for (const std::string &name : NamesIn(root + "/example.com"))
		CHECK(!Contains(name, "second"));

	/* A root that is no directory cannot be listed; that is no empty list. */
	WriteLocal(g_dir + "/not-a-root", "");
	CHECK(ListRepositories(g_dir + "/not-a-root").status == Status::Failed);
}

/* A failed clone deletes its own temp directory and no other: another
 * server may be cloning into the one next to it. Nothing listens on port
 * 1, so the clone fails at once. */
void TestCloneTemps()
{
	std::string error;
	std::optional<Remote> remote = ParseRemoteUrl("https://127.0.0.1:1/team/unreachable", error);
	std::string path = RepositoryPath(g_dir + "/root", *remote);
	Shell("mkdir -p 'root/127.0.0.1:1/team/unreachable.git+tmp-others'");
	std::atomic<bool> abort{false};
	CHECK(!Clone(path, *remote, 1024 * 1024, abort).ok());
	std::vector<std::string> names = NamesIn(g_dir + "/root/127.0.0.1:1/team");
	CHECK(names.size() == 1 && names[0] == "unreachable.git+tmp-others");
	CHECK(!Exists(path));
}

/* A clone aborted by an unloading extension tells so, rather than what
 * libgit2 made of the callback's refusal, and leaves nothing behind. */
void TestAbortedClone()
{
	std::string error;
	std::optional<Remote> remote = ParseRemoteUrl("https://127.0.0.1:1/team/aborted", error);
	std::string path = RepositoryPath(g_dir + "/root", *remote);
	std::atomic<bool> abort{true};
	Result<Nothing> cloned = Clone(path, *remote, 1024 * 1024, abort);
	CHECK(cloned.status == Status::Failed && Contains(cloned.error, "aborted"));
	CHECK(!Exists(path));
	std::vector<std::string> names = NamesIn(g_dir + "/root/127.0.0.1:1/team");
	CHECK(names.size() == 1 && names[0] == "unreachable.git+tmp-others");
}

/* Only a lock file that exists is waited for; a lock that cannot be
 * created for another reason is a failure at once. */
void TestLockMessages()
{
	CHECK(IsLockExistsMessage("failed to lock file '/r/refs/heads/main.lock' for writing: "));
	CHECK(IsLockExistsMessage("failed to create locked file '/r/shallow.lock': File exists"));
	CHECK(!IsLockExistsMessage("failed to create locked file '/r/shallow.lock': Permission denied"));
	CHECK(!IsLockExistsMessage("failed to create locked file '/r/shallow.lock': No space left on device"));
	CHECK(!IsLockExistsMessage("failed to create locked file '/r/shallow.lock': Read-only file system"));
	CHECK(!IsLockExistsMessage("unexpected http status code: 404"));
}

/* Lock files a crashed process left are deleted once they are old; fresh
 * ones and other files stay. */
void TestStaleLocks()
{
	Shell("cd root/example.com/team/repo.git"
	      " && touch -d '20 minutes ago' packed-refs.lock refs/heads/old.lock description"
	      " && touch refs/heads/fresh.lock");
	TakeLogLines();
	std::vector<std::string> removed = RemoveStaleLocks(g_repo, std::chrono::minutes(10));
	CHECK(removed.size() == 2);
	CHECK(!ExistsLocal(g_repo + "/packed-refs.lock") && !ExistsLocal(g_repo + "/refs/heads/old.lock"));
	CHECK(ExistsLocal(g_repo + "/refs/heads/fresh.lock") && ExistsLocal(g_repo + "/description"));
	std::vector<LogLine> lines = TakeLogLines();
	CHECK(lines.size() == 2 && lines[0].level == LogLevel::Message && Contains(lines[0].text, ".lock"));
	unlink((g_repo + "/refs/heads/fresh.lock").c_str());
}

/* What a crash left in the root: an interrupted removal goes always, a
 * clone temp once it has not grown for an hour. */
void TestSweep()
{
	Shell("mkdir -p sweep/host/gone.git+removing-abcdef/repository/objects"
	      " sweep/host/old.git+tmp-abcdef/objects/pack sweep/host/young.git+tmp-abcdef/objects/pack"
	      " sweep/host/kept.git/objects"
	      " && touch -d '2 hours ago' sweep/host/old.git+tmp-abcdef/objects/pack/pack_git2_1"
	      "    sweep/host/old.git+tmp-abcdef/objects/pack sweep/host/old.git+tmp-abcdef"
	      " && touch -d '2 hours ago' sweep/host/young.git+tmp-abcdef/objects/pack sweep/host/young.git+tmp-abcdef"
	      " && touch sweep/host/young.git+tmp-abcdef/objects/pack/pack_git2_1");
	SweepReport report = SweepLeftovers(g_dir + "/sweep");
	CHECK(report.removed.size() == 2 && report.problems.empty());
	std::vector<std::string> names = NamesIn(g_dir + "/sweep/host");
	CHECK(names.size() == 2);
	CHECK(ExistsLocal(g_dir + "/sweep/host/young.git+tmp-abcdef") && ExistsLocal(g_dir + "/sweep/host/kept.git"));
	CHECK(SweepLeftovers(g_dir + "/no-root").problems.empty());
}

/* The names below the root: what each kind of directory is called, and
 * the temp directories next to a repository. */
void TestRootNames()
{
	CHECK(ClassifyRootEntry("repo.git") == RootEntryKind::Repository);
	CHECK(ClassifyRootEntry(".git") == RootEntryKind::Repository);
	CHECK(ClassifyRootEntry("repo.git+tmp-abcdef") == RootEntryKind::CloneTemp);
	CHECK(ClassifyRootEntry("repo.git+removing-abcdef") == RootEntryKind::Removal);
	for (const char *name : {"github.com", "repo.gitx", "repo.git-tmp", "repo", "localhost:8443"})
		CHECK(ClassifyRootEntry(name) == RootEntryKind::Directory);

	std::string error;
	std::optional<Remote> remote = ParseRemoteUrl("https://example.com/team/names", error);
	std::string root = g_dir + "/names";
	std::string path = RepositoryPath(root, *remote);
	CHECK_EQUAL(RepositoryNameAt(root, path), "example.com/team/names");

	Shell("mkdir -p names/example.com/team");
	Result<std::string> temp = CreateCloneTemp(path);
	Result<std::string> otherTemp = CreateCloneTemp(path);
	Result<std::string> removal = CreateRemovalDirectory(path);
	CHECK(temp.ok() && otherTemp.ok() && removal.ok());
	CHECK(temp.value != otherTemp.value);
	CHECK(temp.value.size() == (root + "/example.com/team/names.git+tmp-").size() + 6);
	CHECK(removal.value.size() == (root + "/example.com/team/names.git+removing-").size() + 6);
	CHECK(ExistsLocal(temp.value) && ExistsLocal(otherTemp.value) && ExistsLocal(removal.value));
	CHECK(ClassifyRootEntry(std::filesystem::path(temp.value).filename()) == RootEntryKind::CloneTemp);
	CHECK(ClassifyRootEntry(std::filesystem::path(removal.value).filename()) == RootEntryKind::Removal);
	CHECK_EQUAL(RemovedRepositoryIn(removal.value), removal.value + "/repository");
	CHECK(!CreateCloneTemp(g_dir + "/no-such/dir/x.git").ok());
}

/* The walk through the root finds repositories, clone temps and removals
 * on a plain directory tree, looks into none of them, skips files and stops
 * at the root's depth limit. */
void TestWalkRoot()
{
	std::string deep = "walk";
	for (int i = 0; i < 32; i++)
		deep += "/d";
	Shell("mkdir -p walk/host/a.git/nested.git walk/host/b.git+tmp-abcdef/inner.git"
	      " walk/host/c.git+removing-abcdef/repository walk/host/team/d.git"
	      " " + deep + "/deep.git " + deep + "/d/deeper.git"
	      " && touch walk/host/file.git");
	std::set<std::string> found;
	std::error_code failure = WalkRoot(g_dir + "/walk", [&found](const RootEntry &entry) {
		std::string kind = entry.kind == RootEntryKind::Repository  ? "repository"
		                   : entry.kind == RootEntryKind::CloneTemp ? "clone temp"
		                   : entry.kind == RootEntryKind::Removal   ? "removal"
		                                                            : "directory";
		found.insert(kind + " " + entry.path.substr(g_dir.size() + 1));
	});
	CHECK(!failure);
	std::set<std::string> expected = {"repository walk/host/a.git", "clone temp walk/host/b.git+tmp-abcdef",
	                                  "removal walk/host/c.git+removing-abcdef", "repository walk/host/team/d.git",
	                                  "repository " + deep + "/deep.git"};
	CHECK(found == expected);

	/* What the visit deletes is not looked into afterwards. */
	std::vector<std::string> deleted;
	WalkRoot(g_dir + "/walk", [&deleted](const RootEntry &entry) {
		std::error_code ignored;
		std::filesystem::remove_all(entry.path, ignored);
		deleted.push_back(entry.path);
	});
	CHECK(deleted.size() == expected.size() && !ExistsLocal(g_dir + "/walk/host/a.git"));

	CHECK(WalkRoot(g_dir + "/no-root", [](const RootEntry &) {}) == std::errc::no_such_file_or_directory);
	WriteLocal(g_dir + "/walk-file", "");
	CHECK(WalkRoot(g_dir + "/walk-file", [](const RootEntry &) {}));
}

/* A point of the watches' clock, seconds after its epoch. */
WatchClock::time_point At(int seconds)
{
	return WatchClock::time_point() + std::chrono::seconds(seconds);
}

Remote WatchedRemote(const std::string &url = "https://example.com/team/watched")
{
	std::string error;
	return *ParseRemoteUrl(url, error);
}

RefSnapshot Snapshot(const std::string &headSha, std::vector<Ref> refs = {})
{
	return RefSnapshot{"main", headSha, std::move(refs)};
}

Result<FetchReport> Fetched(const RefSnapshot &refs)
{
	return Ok(FetchReport{false, true, refs});
}

std::string DueNames(WatchSchedule &schedule, int seconds)
{
	std::string names;
	for (const DuePoll &due : schedule.TakeDue(At(seconds)))
		names += (names.empty() ? "" : " ") + due.repoName;
	return names;
}

const int kFirstWatcher = 1;
const int kSecondWatcher = 2;
const Watcher kFirst = &kFirstWatcher;
const Watcher kSecond = &kSecondWatcher;
const char kWatchedName[] = "example.com/team/watched";

/* Interval 0 is watch_interval, shorter ones are raised to 60 seconds, a
 * longer one than watch_interval stands, and a ref watched by several
 * plugins and a repository with several refs go at the shortest. */
void TestWatchIntervals()
{
	WatchSchedule schedule(300);
	Remote remote = WatchedRemote();
	schedule.Watch(remote, "/root/w.git", "HEAD", kFirst, 0, At(0));
	schedule.Watch(remote, "/root/w.git", "main", kFirst, 10, At(0));
	schedule.Watch(remote, "/root/w.git", "v1", kFirst, -5, At(0));
	schedule.Watch(remote, "/root/w.git", "long", kFirst, 900, At(0));
	CHECK_EQUAL(schedule.Describe(kWatchedName), "HEAD (300 s), long (900 s), main (60 s), v1 (60 s)");

	WatchSchedule shared(300);
	shared.Watch(remote, "/root/w.git", "main", kFirst, 600, At(0));
	shared.Watch(remote, "/root/w.git", "main", kSecond, 120, At(0));
	CHECK_EQUAL(shared.Describe(kWatchedName), "main (120 s)");
	CHECK_EQUAL(DueNames(shared, 0), kWatchedName);
	shared.FinishPoll(kWatchedName, Fetched(Snapshot("a")), At(0));
	CHECK_EQUAL(DueNames(shared, 119), "");
	CHECK_EQUAL(DueNames(shared, 120), kWatchedName);
	shared.FinishPoll(kWatchedName, Fetched(Snapshot("a")), At(120));
	shared.Unwatch(kWatchedName, "main", kSecond);
	CHECK_EQUAL(shared.Describe(kWatchedName), "main (600 s)");
	CHECK_EQUAL(DueNames(shared, 240), kWatchedName);
	shared.FinishPoll(kWatchedName, Fetched(Snapshot("a")), At(240));
	CHECK_EQUAL(DueNames(shared, 839), "");
	CHECK_EQUAL(DueNames(shared, 840), kWatchedName);
}

/* A ref nobody watched yet is fetched at once, once at a time, and that
 * fetch only notes where it is. A ref watched already keeps the timer,
 * which only comes sooner for a shorter interval than what is left of
 * it. */
void TestWatchTimer()
{
	WatchSchedule schedule(300);
	Remote remote = WatchedRemote();
	schedule.Watch(remote, "/root/w.git", "main", kFirst, 0, At(1000));
	std::vector<DuePoll> due = schedule.TakeDue(At(1000));
	CHECK(due.size() == 1 && due[0].repoName == kWatchedName && due[0].path == "/root/w.git");
	CHECK_EQUAL(DueNames(schedule, 5000), "");
	std::vector<Ref> refs = {Ref{"main", "a", false}};
	CHECK(schedule.FinishPoll(kWatchedName, Fetched(Snapshot("a", refs)), At(1010)).empty());

	schedule.Watch(remote, "/root/w.git", "main", kSecond, 600, At(1020));
	CHECK_EQUAL(DueNames(schedule, 1309), "");
	schedule.Watch(remote, "/root/w.git", "main", kSecond, 60, At(1100));
	CHECK_EQUAL(DueNames(schedule, 1159), "");
	CHECK_EQUAL(DueNames(schedule, 1160), kWatchedName);
	schedule.FinishPoll(kWatchedName, Fetched(Snapshot("a", refs)), At(1160));

	schedule.Watch(remote, "/root/w.git", "v1", kFirst, 0, At(1170));
	CHECK_EQUAL(DueNames(schedule, 1170), kWatchedName);
}

/* A watched ref at another commit than when it was last seen is a move,
 * also when it went away or appeared; a name that is a branch and a tag
 * follows the branch, HEAD the default branch. */
void TestWatchMoves()
{
	WatchSchedule schedule(300);
	Remote remote = WatchedRemote();
	for (const char *ref : {"HEAD", "main", "v1", "both"})
		schedule.Watch(remote, "/root/w.git", ref, kFirst, 0, At(0));
	RefSnapshot first = Snapshot("h1", {Ref{"main", "m1", false}, Ref{"both", "t1", true}, Ref{"both", "b1", false}});
	CHECK(schedule.Observe(kWatchedName, first).empty());
	CHECK(schedule.Observe(kWatchedName, first).empty());

	RefSnapshot second = Snapshot("h2", {Ref{"v1", "t2", true}, Ref{"both", "t3", true}, Ref{"both", "b1", false}});
	std::vector<RefMove> moves = schedule.Observe(kWatchedName, second);
	std::string told;
	for (const RefMove &move : moves) {
		CHECK_EQUAL(move.url, remote.url);
		told += move.ref + ":" + move.oldSha + ">" + move.newSha + " ";
	}
	CHECK_EQUAL(told, "HEAD:h1>h2 main:m1> v1:>t2 ");

	CHECK_EQUAL(ShaOfWatchedRef(second, "both"), "b1");
	CHECK_EQUAL(ShaOfWatchedRef(second, "v1"), "t2");
	CHECK_EQUAL(ShaOfWatchedRef(second, "HEAD"), "h2");
	CHECK_EQUAL(ShaOfWatchedRef(second, "gone"), "");
	CHECK(schedule.Observe("example.com/team/other", second).empty());
}

/* The refs on disk are compared also after a failed fetch, since another
 * server sharing the root may have fetched; the timer goes on either way. */
void TestWatchFailedPoll()
{
	WatchSchedule schedule(300);
	schedule.Watch(WatchedRemote(), "/root/w.git", "main", kFirst, 60, At(0));
	schedule.Observe(kWatchedName, Snapshot("a", {Ref{"main", "a", false}}));

	CHECK_EQUAL(DueNames(schedule, 0), kWatchedName);
	Result<FetchReport> failed = Fail<FetchReport>(Status::Network, "unreachable");
	failed.value = FetchReport{false, true, Snapshot("a", {Ref{"main", "b", false}})};
	std::vector<RefMove> moves = schedule.FinishPoll(kWatchedName, failed, At(10));
	CHECK(moves.size() == 1 && moves[0].oldSha == "a" && moves[0].newSha == "b");

	CHECK_EQUAL(DueNames(schedule, 70), kWatchedName);
	Result<FetchReport> gone = Fail<FetchReport>(Status::NotCloned, "not cloned");
	CHECK(schedule.FinishPoll(kWatchedName, gone, At(70)).empty());
	CHECK_EQUAL(DueNames(schedule, 129), "");
	CHECK_EQUAL(DueNames(schedule, 130), kWatchedName);
}

/* A watch ends with Unwatch or the watcher's unload; the repository is
 * watched as long as any of its refs is. */
void TestWatchEnds()
{
	WatchSchedule schedule(300);
	Remote remote = WatchedRemote();
	Remote other = WatchedRemote("https://example.com/team/other");
	schedule.Watch(remote, "/root/w.git", "main", kFirst, 0, At(0));
	schedule.Watch(remote, "/root/w.git", "main", kSecond, 0, At(0));
	schedule.Watch(remote, "/root/w.git", "v1", kFirst, 0, At(0));
	schedule.Watch(other, "/root/o.git", "main", kFirst, 0, At(0));
	CHECK(schedule.WatchersOf(kWatchedName).size() == 2);

	schedule.Unwatch(kWatchedName, "main", kFirst);
	schedule.Unwatch(kWatchedName, "nothing", kFirst);
	schedule.Unwatch("example.com/nothing", "main", kFirst);
	CHECK_EQUAL(schedule.Describe(kWatchedName), "main (300 s), v1 (300 s)");
	schedule.Unwatch(kWatchedName, "main", kSecond);
	CHECK_EQUAL(schedule.Describe(kWatchedName), "v1 (300 s)");
	CHECK(schedule.WatchersOf(kWatchedName) == std::vector<Watcher>{kFirst});

	schedule.Watch(other, "/root/o.git", "main", kSecond, 0, At(0));
	schedule.Forget(kFirst);
	CHECK(!schedule.IsWatched(kWatchedName) && schedule.WatchersOf(kWatchedName).empty());
	CHECK(schedule.IsWatched(other.name) && schedule.WatchersOf(other.name) == std::vector<Watcher>{kSecond});
	CHECK(schedule.FinishPoll(kWatchedName, Fetched(Snapshot("a")), At(0)).empty());
	CHECK_EQUAL(schedule.Describe(kWatchedName), "");
}

/* A task's exception becomes a failure for its plugin; one that escapes
 * the task anyway is logged and costs only its own answer. */
void TestExceptions()
{
	Result<int> thrown = Guarded([]() -> Result<int> { throw std::runtime_error("boom"); });
	CHECK(thrown.status == Status::Failed && Contains(thrown.error, "boom"));
	Result<int> odd = Guarded([]() -> Result<int> { throw 42; });
	CHECK(odd.status == Status::Failed);
	CHECK(Guarded([] { return Ok(7); }).value == 7);

	TakeLogLines();
	std::vector<int> delivered;
	Worker worker;
	worker.Submit([](const std::atomic<bool> &) -> Completion { throw std::runtime_error("escaped"); });
	worker.Submit([&delivered](const std::atomic<bool> &) -> Completion {
		return [&delivered] { delivered.push_back(1); };
	});
	for (int waited = 0; delivered.empty() && waited < 5000; waited += 10) {
		for (const Completion &completion : worker.TakeCompletions())
			completion();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	worker.Stop();
	CHECK(delivered.size() == 1);
	std::vector<LogLine> lines = TakeLogLines();
	CHECK(lines.size() == 1 && lines[0].level == LogLevel::Error && Contains(lines[0].text, "escaped"));
}

void TestWorker()
{
	std::vector<int> ran;
	std::vector<int> delivered;
	Worker worker;
	for (int i = 0; i < 20; i++) {
		worker.Submit([&ran, &delivered, i](const std::atomic<bool> &) -> Completion {
			ran.push_back(i);
			return [&delivered, i] { delivered.push_back(i); };
		});
	}

	for (int waited = 0; delivered.size() < 20 && waited < 5000; waited += 10) {
		for (const Completion &completion : worker.TakeCompletions())
			completion();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	worker.Stop();
	CHECK(ran.size() == 20 && delivered.size() == 20);
	for (size_t i = 0; i < delivered.size(); i++)
		CHECK(ran[i] == static_cast<int>(i) && delivered[i] == static_cast<int>(i));

	worker.Submit([&ran](const std::atomic<bool> &) -> Completion { ran.push_back(99); return [] {}; });
	CHECK(ran.size() == 20 && worker.TakeCompletions().empty());
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <CA bundle>\n", argv[0]);
		return 2;
	}
	char directory[] = "/tmp/git-tests-XXXXXX";
	if (!mkdtemp(directory)) {
		perror("mkdtemp");
		return 2;
	}
	g_dir = directory;

	CHECK(!Initialize(Config{}, "/etc/hostname").empty());
	std::string failure = Initialize(Config{}, argv[1]);
	if (!failure.empty()) {
		fprintf(stderr, "%s\n", failure.c_str());
		return 2;
	}

	MakeFixture();
	TestRemoteUrls();
	TestRepoPaths();
	TestGamePaths();
	TestEntryNames();
	TestWatchableRefs();
	TestConfig();
	TestResolveAndRefs();
	TestLog();
	TestDiff();
	TestTree();
	TestReadFile();
	TestWriteFile();
	TestWriteDir();
	TestDestinationSymlinks();
	TestWriteErrors();
	TestWriteTempSymlink();
	TestWriteTempNames();
	TestUnsafeEntryNames();
	TestTreeBounds();
	TestCompareFile();
	TestCompareFileLinks();
	TestListAndRemove();
	TestCloneTemps();
	TestAbortedClone();
	TestLockMessages();
	TestStaleLocks();
	TestSweep();
	TestRootNames();
	TestWalkRoot();
	TestWatchIntervals();
	TestWatchTimer();
	TestWatchMoves();
	TestWatchFailedPoll();
	TestWatchEnds();
	TestWorker();
	TestExceptions();
	Shutdown();

	std::string cleanup = "rm -rf '" + g_dir + "'";
	if (system(cleanup.c_str()) != 0)
		fprintf(stderr, "could not remove %s\n", g_dir.c_str());
	if (g_failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all git tests passed\n");
	return 0;
}

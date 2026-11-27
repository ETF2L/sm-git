/**
 * Copyright 2026 ETF2L Contributors, Dave
 *
 * The tests of git.inc, run with smrunner (sm-runner) against the built
 * extension and sm-git's own repository on GitHub:
 *
 *     smrunner test test_git.smx --extensions <dist>/extensions \
 *         --file <dist>/configs/git/ca-bundle.crt=addons/sourcemod/configs/git/ca-bundle.crt
 *
 * Every test runs on a fresh server and clones https://github.com/ETF2L/sm-git
 * into its own root, so the tests need the network. They read two fixed
 * revisions: the tag v1.0.0 (the first commit) and the tag smrunner-fixture
 * (the commit that added these tests, whose parent is v1.0.0).
 */

// no critic (ProhibitLongFiles)
#pragma semicolon 1
#pragma newdecls required
#pragma dynamic 131072

#include <sourcemod>
#include <git>
#include <smrunner>

#define URL     "https://github.com/ETF2L/sm-git"
#define NAME    "github.com/ETF2L/sm-git"
#define V1      "65c9346d055dd9c3ec8294831ed3ca59e830792f"
#define FIXTURE "smrunner-fixture"

/* ------------------------------------------------------------------ helpers */

/* Runs failing, which must stop with a native error containing text. */
void ExpectNativeError(Function failing, const char[] text) {
	Runner.ExpectError(text);
	Call_StartFunction(null, failing);
	Call_Finish();
}

/* What the last callback reported. */
int g_calls;
GitStatus g_status;
Handle g_result;
bool g_flag;
GitFileState g_state;
char g_text[65536];
int g_length;
char g_error[512];

void Reported(GitStatus status, Handle result, const char[] error) {
	g_status = status;
	g_result = result;
	strcopy(g_error, sizeof(g_error), error);
	g_calls++;
}

bool HasCalledBack(any count) {
	return g_calls >= count;
}

/* Waits for the next callback, which must report expected. */
void Await(GitStatus expected = Git_OK) {
	int before = g_calls;
	Assert.True(Runner.WaitUntil(HasCalledBack, 60.0, before + 1), "the callback came");
	Assert.Equal(g_status, expected, "status %d, error \"%s\"", g_status, g_error);
}

void OnRepo(GitStatus status, GitRepo repo, bool cloned, const char[] error, any data) {
	g_flag = cloned;
	Reported(status, repo, error);
}

void OnFetched(GitStatus status, bool changed, const char[] error, any data) {
	g_flag = changed;
	Reported(status, null, error);
}

void OnText(GitStatus status, const char[] text, const char[] error, any data) {
	strcopy(g_text, sizeof(g_text), text);
	Reported(status, null, error);
}

void OnRead(GitStatus status, const char[] content, int length, const char[] error, any data) {
	strcopy(g_text, sizeof(g_text), content);
	g_length = length;
	Reported(status, null, error);
}

void OnCompared(GitStatus status, GitFileState state, const char[] error, any data) {
	g_state = state;
	Reported(status, null, error);
}

void OnStatus(GitStatus status, const char[] error, any data) {
	Reported(status, null, error);
}

void OnRefs(GitStatus status, GitRefList refs, const char[] error, any data) {
	Reported(status, refs, error);
}

void OnLog(GitStatus status, GitCommitList commits, const char[] error, any data) {
	Reported(status, commits, error);
}

void OnDiff(GitStatus status, GitDiff diff, const char[] error, any data) {
	Reported(status, diff, error);
}

void OnTree(GitStatus status, GitTree tree, const char[] error, any data) {
	Reported(status, tree, error);
}

void OnWritten(GitStatus status, GitChanges changes, const char[] error, any data) {
	Reported(status, changes, error);
}

void OnRepos(GitStatus status, GitRepoList repos, const char[] error, any data) {
	Reported(status, repos, error);
}

/* Opens sm-git's repository, cloning it; fails unless that worked. */
GitRepo OpenRepo(const char[] url = URL) {
	GitRepo.Open(url, OnRepo);
	Await();
	Assert.True(g_result != null, "Git_OK gives a handle");
	return view_as<GitRepo>(g_result);
}

/* The full SHA of a revision of repo. */
void ResolveTo(GitRepo repo, const char[] rev, char[] sha, int maxlength) {
	repo.Resolve(rev, OnText);
	Await();
	strcopy(sha, maxlength, g_text);
}

/* The index of path in a tree, or -1. */
int FindInTree(GitTree tree, const char[] path) {
	for (int i = 0; i < tree.Length; i++) {
		char entry[PLATFORM_MAX_PATH];
		tree.GetPath(i, entry, sizeof(entry));
		if (StrEqual(entry, path)) {
			return i;
		}
	}
	return -1;
}

/* The index of path in a diff, or -1. */
int FindInDiff(GitDiff diff, const char[] path) {
	for (int i = 0; i < diff.Length; i++) {
		char entry[PLATFORM_MAX_PATH];
		diff.GetPath(i, entry, sizeof(entry));
		if (StrEqual(entry, path)) {
			return i;
		}
	}
	return -1;
}

/* The index of path in changes, or -1. */
int FindInChanges(GitChanges changes, const char[] path) {
	for (int i = 0; i < changes.Length; i++) {
		char entry[PLATFORM_MAX_PATH];
		changes.GetPath(i, entry, sizeof(entry));
		if (StrEqual(entry, path)) {
			return i;
		}
	}
	return -1;
}

void WriteTextFile(const char[] path, const char[] text) {
	File file = OpenFile(path, "wb");
	Assert.True(file != null, "%s can be written", path);
	file.WriteString(text, false);
	delete file;
}

/* ------------------------------------------------------------- opening */

public void Test_OpenClonesTheRepositoryOnce() {
	GitRepo repo = OpenRepo();
	Assert.True(g_flag, "the first open cloned it");
	Assert.True(DirExists("addons/sourcemod/data/git"), "below the default root");

	GitRepo again = OpenRepo();
	Assert.False(g_flag, "the second open found it");
	delete again;
	delete repo;
}

public void Test_OpenWithoutCloneTellsAMissingRepository() {
	GitRepo.Open(URL, OnRepo, 0, false);
	Await(Git_NotCloned);
	Assert.Equal(g_result, INVALID_HANDLE, "no handle without Git_OK");
}

public void Test_UrlsAreNormalized() {
	GitRepo repo = OpenRepo("https://GitHub.com/ETF2L/sm-git.git/");
	char text[256];
	Assert.Equal(repo.GetUrl(text, sizeof(text)), strlen(URL));
	Assert.StrEqual(text, URL);
	repo.GetName(text, sizeof(text));
	Assert.StrEqual(text, NAME);
	delete repo;
}

public void Test_OpenRefusesUrlsThatBreakTheRules() {
	char urls[][] = {
		"http://github.com/ETF2L/sm-git", "ssh://github.com/ETF2L/sm-git", "git@github.com:ETF2L/sm-git",
		"HTTPS://github.com/ETF2L/sm-git", "https://user@github.com/ETF2L/sm-git", "https://github.com:123456/x",
		"https://github.com/a?b", "https://github.com/a//b", "https://github.com/a/../b",
		"https://github.com/a.git/b", "https://github.com/owner/.git", "cfg/local",
	};
	for (int i = 0; i < sizeof(urls); i++) {
		GitRepo.Open(urls[i], OnRepo);
		Await(Git_Refused);
		Assert.Equal(g_result, INVALID_HANDLE, "%s", urls[i]);
	}
}

public void Test_FetchFindsNothingNewRightAfterTheClone() {
	GitRepo repo = OpenRepo();
	repo.Fetch(OnFetched);
	Await();
	Assert.False(g_flag, "nothing changed");
	delete repo;
}

/* ----------------------------------------------------------- revisions */

public void Test_ResolveGivesTheFullSha() {
	GitRepo repo = OpenRepo();
	char sha[64];
	ResolveTo(repo, "v1.0.0", sha, sizeof(sha));
	Assert.StrEqual(sha, V1);
	ResolveTo(repo, "65c9346", sha, sizeof(sha));
	Assert.StrEqual(sha, V1, "an abbreviated SHA");
	ResolveTo(repo, FIXTURE ... "^", sha, sizeof(sha));
	Assert.StrEqual(sha, V1, "the fixture's parent is v1.0.0");
	ResolveTo(repo, "HEAD", sha, sizeof(sha));
	Assert.Equal(strlen(sha), 40);

	repo.Resolve("no-such-branch", OnText);
	Await(Git_NotFound);
	delete repo;
}

public void Test_ListRefsHasTheBranchesAndTags() {
	GitRepo repo = OpenRepo();
	repo.ListRefs(OnRefs);
	Await();
	GitRefList refs = view_as<GitRefList>(g_result);

	char head[64];
	refs.GetHeadBranch(head, sizeof(head));
	Assert.StrEqual(head, "main");
	char headSha[64];
	Assert.Equal(refs.GetHeadSha(headSha, sizeof(headSha)), 40);

	bool foundTag = false;
	bool foundMain = false;
	for (int i = 0; i < refs.Length; i++) {
		char name[128];
		refs.GetName(i, name, sizeof(name));
		char sha[64];
		refs.GetSha(i, sha, sizeof(sha));
		if (StrEqual(name, "v1.0.0")) {
			foundTag = true;
			Assert.True(refs.IsTag(i));
			Assert.StrEqual(sha, V1, "the commit the tag points to");
		} else if (StrEqual(name, "main")) {
			foundMain = true;
			Assert.False(refs.IsTag(i));
			Assert.StrEqual(sha, headSha);
		}
	}
	Assert.True(foundTag, "the tag v1.0.0 is listed");
	Assert.True(foundMain, "the branch main is listed");
	delete refs;
	delete repo;
}

public void Test_LogListsTheCommitsNewestFirst() {
	GitRepo repo = OpenRepo();
	repo.Log("v1.0.0", OnLog);
	Await();
	GitCommitList commits = view_as<GitCommitList>(g_result);
	Assert.Equal(commits.Length, 1, "v1.0.0 is the first commit");
	char text[256];
	commits.GetSha(0, text, sizeof(text));
	Assert.StrEqual(text, V1);
	commits.GetAuthorName(0, text, sizeof(text));
	Assert.StrEqual(text, "davenonymous");
	commits.GetAuthorEmail(0, text, sizeof(text));
	Assert.StrEqual(text, "dave@davenonymous.com");
	Assert.Equal(commits.GetTime(0), 1795518023);
	commits.GetSummary(0, text, sizeof(text));
	Assert.StrEqual(text, "sm-git: Initial commit");
	commits.GetMessage(0, text, sizeof(text));
	Assert.StrContains(text, "sm-git: Initial commit");
	delete commits;

	repo.Log(FIXTURE, OnLog, 0, "", 2);
	Await();
	commits = view_as<GitCommitList>(g_result);
	Assert.Equal(commits.Length, 2);
	commits.GetSha(1, text, sizeof(text));
	Assert.StrEqual(text, V1, "the older commit comes second");
	delete commits;
	delete repo;
}

public void Test_LogTakesAPathAndATime() {
	GitRepo repo = OpenRepo();
	repo.Log(FIXTURE, OnLog, 0, "tests");
	Await();
	GitCommitList commits = view_as<GitCommitList>(g_result);
	Assert.Equal(commits.Length, 1, "only the fixture's commit changed tests/");
	char sha[64];
	commits.GetSha(0, sha, sizeof(sha));
	Assert.NotEqual(StrEqual(sha, V1), true);
	delete commits;

	repo.Log(FIXTURE, OnLog, 0, "", 50, 1795518024);
	Await();
	commits = view_as<GitCommitList>(g_result);
	Assert.Equal(commits.Length, 1, "v1.0.0 was authored a second before since");
	delete commits;
	delete repo;
}

public void Test_DiffListsTheChangedFiles() {
	GitRepo repo = OpenRepo();
	repo.Diff("v1.0.0", "v1.0.0", OnDiff);
	Await();
	GitDiff diff = view_as<GitDiff>(g_result);
	Assert.Equal(diff.Length, 0);
	delete diff;

	repo.Diff("v1.0.0", FIXTURE, OnDiff);
	Await();
	diff = view_as<GitDiff>(g_result);

	int added = FindInDiff(diff, "tests/test_git.sp");
	Assert.True(added != -1, "the tests were added");
	Assert.Equal(diff.GetStatus(added), GitDiff_Added);
	char sha[64];
	Assert.Equal(diff.GetOldSha(added, sha, sizeof(sha)), 0, "an added file has no old SHA");
	Assert.Equal(diff.GetNewSha(added, sha, sizeof(sha)), 40);

	int modified = FindInDiff(diff, ".github/workflows/ci.yml");
	Assert.True(modified != -1, "the workflow changed");
	Assert.Equal(diff.GetStatus(modified), GitDiff_Modified);
	diff.GetOldSha(modified, sha, sizeof(sha));
	Assert.StrEqual(sha, "f9f16aa9987bbee25d823f697df6f2b53854e227", "ci.yml's blob in v1.0.0");

	char previous[PLATFORM_MAX_PATH];
	for (int i = 0; i < diff.Length; i++) {
		char path[PLATFORM_MAX_PATH];
		diff.GetPath(i, path, sizeof(path));
		Assert.True(strcmp(previous, path) < 0, "%s comes after %s", path, previous);
		strcopy(previous, sizeof(previous), path);
	}
	delete diff;

	repo.Diff(FIXTURE, "v1.0.0", OnDiff, 0, "tests");
	Await();
	diff = view_as<GitDiff>(g_result);
	Assert.True(diff.Length >= 1);
	for (int i = 0; i < diff.Length; i++) {
		char path[PLATFORM_MAX_PATH];
		diff.GetPath(i, path, sizeof(path));
		Assert.True(StrContains(path, "tests/") == 0, "%s is below tests", path);
		Assert.Equal(diff.GetStatus(i), GitDiff_Deleted);
		Assert.Equal(diff.GetNewSha(i, sha, sizeof(sha)), 0, "a deleted file has no new SHA");
	}
	delete diff;
	delete repo;
}

/* --------------------------------------------------------------- trees */

public void Test_ListTreeListsTheEntries() {
	GitRepo repo = OpenRepo();
	repo.ListTree("v1.0.0", OnTree);
	Await();
	GitTree tree = view_as<GitTree>(g_result);
	Assert.Equal(tree.Length, 53, "41 files and 12 directories");

	int readme = FindInTree(tree, "README.md");
	Assert.Equal(tree.GetType(readme), GitEntry_File);
	Assert.Equal(tree.GetSize(readme), 8370);
	char sha[64];
	tree.GetSha(readme, sha, sizeof(sha));
	Assert.StrEqual(sha, "baeee6a32390385376e6bee6b314a236e7174336");
	Assert.False(tree.IsExecutable(readme));

	int script = FindInTree(tree, "deps/build-deps.sh");
	Assert.True(tree.IsExecutable(script));
	int deps = FindInTree(tree, "deps");
	Assert.Equal(tree.GetType(deps), GitEntry_Directory);
	Assert.Equal(tree.GetSize(deps), 0);
	Assert.True(deps < script, "a directory comes before its entries");
	delete tree;

	repo.ListTree("v1.0.0", OnTree, 0, "", false);
	Await();
	tree = view_as<GitTree>(g_result);
	Assert.Equal(tree.Length, 10, "the top only");
	delete tree;

	repo.ListTree("v1.0.0", OnTree, 0, "configs");
	Await();
	tree = view_as<GitTree>(g_result);
	Assert.Equal(tree.Length, 1);
	char path[PLATFORM_MAX_PATH];
	tree.GetPath(0, path, sizeof(path));
	Assert.StrEqual(path, "configs/git.cfg", "paths are from the top");
	delete tree;

	repo.ListTree("v1.0.0", OnTree, 0, "no-such-dir");
	Await(Git_NotFound);
	delete repo;
}

/* --------------------------------------------------------------- files */

public void Test_ReadFileGivesTheContent() {
	GitRepo repo = OpenRepo();
	repo.ReadFile("v1.0.0", "README.md", OnRead, 0, 16384);
	Await();
	Assert.Equal(g_length, 8370);
	Assert.True(StrContains(g_text, "# sm-git\n") == 0, "README.md starts with its title");

	repo.ReadFile("v1.0.0", "README.md", OnRead, 0, 100);
	Await(Git_TooLarge);
	repo.ReadFile("v1.0.0", "configs", OnRead);
	Await(Git_NotFound);
	repo.ReadFile("v1.0.0", "missing.txt", OnRead);
	Await(Git_NotFound);
	repo.ReadFile("no-such-rev", "README.md", OnRead);
	Await(Git_NotFound);
	delete repo;
}

public void Test_WriteFileWritesOnlyWhenTheContentDiffers() {
	GitRepo repo = OpenRepo();
	repo.WriteFile("v1.0.0", "configs/git.cfg", "cfg/written/git.cfg", OnWritten);
	Await();
	GitChanges changes = view_as<GitChanges>(g_result);
	Assert.Equal(changes.Length, 1);
	char path[PLATFORM_MAX_PATH];
	changes.GetPath(0, path, sizeof(path));
	Assert.StrEqual(path, "cfg/written/git.cfg");
	Assert.Equal(changes.GetKind(0), GitChange_Written);
	delete changes;
	Assert.Equal(FileSize("cfg/written/git.cfg"), 961);

	repo.WriteFile("v1.0.0", "configs/git.cfg", "cfg/written/git.cfg", OnWritten);
	Await();
	changes = view_as<GitChanges>(g_result);
	Assert.Equal(changes.Length, 0, "the file had the content already");
	delete changes;

	WriteTextFile("cfg/written/git.cfg", "changed");
	repo.WriteFile("v1.0.0", "configs/git.cfg", "cfg/written/git.cfg", OnWritten);
	Await();
	changes = view_as<GitChanges>(g_result);
	Assert.Equal(changes.Length, 1, "a changed file is written again");
	delete changes;
	Assert.Equal(FileSize("cfg/written/git.cfg"), 961);
	Assert.False(FileExists("cfg/written/git.cfg.git-tmp"), "no temp file is left");
	delete repo;
}

public void Test_WriteFileGivesChangesAlsoOnFailure() {
	GitRepo repo = OpenRepo();
	repo.WriteFile("v1.0.0", "missing.txt", "cfg/missing.txt", OnWritten);
	Await(Git_NotFound);
	GitChanges changes = view_as<GitChanges>(g_result);
	Assert.True(changes != null, "the changes are valid whatever the status");
	Assert.Equal(changes.Length, 0);
	delete changes;
	delete repo;
}

public void Test_WriteOutsRefuseTheRootAndGitTmpNames() {
	GitRepo repo = OpenRepo();
	repo.WriteFile("v1.0.0", "README.md", "addons/sourcemod/data/git/README.md", OnWritten);
	Await(Git_Refused);
	delete g_result;

	repo.WriteFile("v1.0.0", "README.md", "cfg/a.git-tmp/README.md", OnWritten);
	Await(Git_Refused);
	delete g_result;
	Assert.False(DirExists("cfg/a.git-tmp"));
	delete repo;
}

public void Test_WriteDirWritesAndDeletesWithOldRev() {
	GitRepo repo = OpenRepo();
	repo.WriteDir(FIXTURE, "", "cfg/checkout", OnWritten);
	Await();
	GitChanges changes = view_as<GitChanges>(g_result);
	Assert.Equal(FindInChanges(changes, "cfg/checkout/tests/test_git.sp") != -1, true);
	Assert.Equal(FindInChanges(changes, "cfg/checkout/deps/build-deps.sh") != -1, true);
	delete changes;
	Assert.Equal(FileSize("cfg/checkout/README.md") > 0, true);
	Assert.True(FileExists("cfg/checkout/tests/test_git.sp"));

	WriteTextFile("cfg/checkout/mine.txt", "not from the repository");
	repo.WriteDir("v1.0.0", "", "cfg/checkout", OnWritten, 0, FIXTURE);
	Await();
	changes = view_as<GitChanges>(g_result);
	int deleted = FindInChanges(changes, "cfg/checkout/tests/test_git.sp");
	Assert.True(deleted != -1, "the fixture's tests are gone in v1.0.0");
	Assert.Equal(changes.GetKind(deleted), GitChange_Deleted);
	int rewritten = FindInChanges(changes, "cfg/checkout/.github/workflows/ci.yml");
	Assert.Equal(changes.GetKind(rewritten), GitChange_Written);
	Assert.True(deleted < rewritten, "deletions come first");
	delete changes;

	Assert.False(FileExists("cfg/checkout/tests/test_git.sp"));
	Assert.True(FileExists("cfg/checkout/mine.txt"), "a file the repository never had stays");
	delete repo;
}

public void Test_WriteDirWithoutOldRevDeletesNothing() {
	GitRepo repo = OpenRepo();
	repo.WriteDir(FIXTURE, "tests", "cfg/tests", OnWritten);
	Await();
	delete g_result;
	repo.WriteDir("v1.0.0", "configs", "cfg/tests", OnWritten);
	Await();
	delete g_result;
	Assert.True(FileExists("cfg/tests/test_git.sp"), "nothing was deleted");
	Assert.True(FileExists("cfg/tests/git.cfg"));

	repo.WriteDir("v1.0.0", "tests", "cfg/none", OnWritten);
	Await(Git_NotFound);
	delete g_result;
	delete repo;
}

public void Test_CompareFileTellsTheState() {
	GitRepo repo = OpenRepo();
	repo.CompareFile("v1.0.0", "configs/git.cfg", "cfg/compare.cfg", OnCompared);
	Await();
	Assert.Equal(g_state, GitFile_Missing);

	repo.WriteFile("v1.0.0", "configs/git.cfg", "cfg/compare.cfg", OnWritten);
	Await();
	delete g_result;
	repo.CompareFile("v1.0.0", "configs/git.cfg", "cfg/compare.cfg", OnCompared);
	Await();
	Assert.Equal(g_state, GitFile_Same);

	WriteTextFile("cfg/compare.cfg", "other");
	repo.CompareFile("v1.0.0", "configs/git.cfg", "cfg/compare.cfg", OnCompared);
	Await();
	Assert.Equal(g_state, GitFile_Different);

	repo.CompareFile("v1.0.0", "missing.cfg", "cfg/compare.cfg", OnCompared);
	Await(Git_NotFound);
	Assert.Equal(g_state, GitFile_Different, "Different whenever the status is not Git_OK");
	delete repo;
}

/* ------------------------------------------------- watches and removal */

public void Test_ListRepositoriesTellsWhoUsesThem() {
	Git_ListRepositories(OnRepos);
	Await();
	GitRepoList repos = view_as<GitRepoList>(g_result);
	Assert.Equal(repos.Length, 0, "a root that does not exist yet");
	delete repos;

	GitRepo repo = OpenRepo();
	repo.Watch("main", 600);
	Git_ListRepositories(OnRepos);
	Await();
	repos = view_as<GitRepoList>(g_result);
	Assert.Equal(repos.Length, 1);
	char text[256];
	repos.GetName(0, text, sizeof(text));
	Assert.StrEqual(text, NAME);
	repos.GetUrl(0, text, sizeof(text));
	Assert.StrEqual(text, URL);
	repos.GetHeadBranch(0, text, sizeof(text));
	Assert.StrEqual(text, "main");
	Assert.Equal(repos.GetHeadSha(0, text, sizeof(text)), 40);
	repos.GetOpeners(0, text, sizeof(text));
	Assert.StrEqual(text, "test_git.smx");
	repos.GetWatchers(0, text, sizeof(text));
	Assert.StrEqual(text, "main (600 s)");
	Assert.True(repos.GetFetchTime(0) > 0, "the clone counts as a fetch");
	delete repos;

	repo.Unwatch("main");
	delete repo;
}

public void Test_RemoveIsRefusedWhileTheRepositoryIsUsed() {
	GitRepo repo = OpenRepo();
	GitRepo other = OpenRepo();
	repo.Remove(OnStatus);
	Await(Git_InUse);
	Assert.StrContains(g_error, "test_git.smx");
	delete other;

	repo.Watch("main");
	repo.Remove(OnStatus);
	Await(Git_InUse);
	repo.Unwatch("main");

	repo.Remove(OnStatus);
	Await();
	Assert.False(DirExists("addons/sourcemod/data/git/github.com/ETF2L/sm-git.git"));

	repo.ListRefs(OnRefs);
	Await(Git_NotCloned);
	GitRepo.Open(URL, OnRepo, 0, false);
	Await(Git_NotCloned);
	delete repo;
}

/* -------------------------------------------------------- native errors */

GitRepo g_repo;
Handle g_list;

public void Test_NativesRefuseBadArguments() {
	g_repo = OpenRepo();
	char failing[][] = {
		"Call_OpenWithoutCallback", "Call_ResolveEmpty", "Call_LogMaxZero", "Call_LogMaxTooLarge",
		"Call_LogBadPath", "Call_DiffEmptyHead", "Call_TreeDotDot", "Call_ReadEmptyPath", "Call_ReadMaxBytesZero",
		"Call_WriteBackslashDest", "Call_WriteAbsoluteDest", "Call_WriteDirGitPart", "Call_CompareEmptyFile",
		"Call_WatchFullRefName", "Call_WatchExpression", "Call_WatchNegativeInterval", "Call_ListWithoutCallback",
	};
	char texts[][] = {
		"Invalid callback function", "must name a revision", "max 0 is outside 1-1000", "max 1001 is outside 1-1000",
		"must be a path inside the repository", "must name a revision", "must be a path inside the repository",
		"must be a path inside the repository", "maxBytes 0 must be positive",
		"must be a non-empty path relative to the game directory",
		"must be a non-empty path relative to the game directory", "must be a path inside the repository",
		"must be a non-empty path relative to the game directory",
		"must be HEAD or the short name of a branch or tag", "must be HEAD or the short name of a branch or tag",
		"interval -1 must not be negative", "Invalid callback function",
	};
	for (int i = 0; i < sizeof(failing); i++) {
		ExpectNativeError(GetFunctionByName(null, failing[i]), texts[i]);
	}
	Runner.Frames(5);
	Assert.Equal(g_calls, 1, "only the open called back");
	delete g_repo;
}

public void Call_OpenWithoutCallback() {
	GitRepo.Open(URL, view_as<GitRepoCallback>(INVALID_FUNCTION));
}

public void Call_ResolveEmpty() {
	g_repo.Resolve("", OnText);
}

public void Call_LogMaxZero() {
	g_repo.Log("HEAD", OnLog, 0, "", 0);
}

public void Call_LogMaxTooLarge() {
	g_repo.Log("HEAD", OnLog, 0, "", 1001);
}

public void Call_LogBadPath() {
	g_repo.Log("HEAD", OnLog, 0, "a//b");
}

public void Call_DiffEmptyHead() {
	g_repo.Diff("HEAD", "", OnDiff);
}

public void Call_TreeDotDot() {
	g_repo.ListTree("HEAD", OnTree, 0, "../x");
}

public void Call_ReadEmptyPath() {
	g_repo.ReadFile("HEAD", "", OnRead);
}

public void Call_ReadMaxBytesZero() {
	g_repo.ReadFile("HEAD", "README.md", OnRead, 0, 0);
}

public void Call_WriteBackslashDest() {
	g_repo.WriteFile("HEAD", "README.md", "cfg\\README.md", OnWritten);
}

public void Call_WriteAbsoluteDest() {
	g_repo.WriteFile("HEAD", "README.md", "/tmp/README.md", OnWritten);
}

public void Call_WriteDirGitPart() {
	g_repo.WriteDir("HEAD", ".git", "cfg/x", OnWritten);
}

public void Call_CompareEmptyFile() {
	g_repo.CompareFile("HEAD", "README.md", "", OnCompared);
}

public void Call_WatchFullRefName() {
	g_repo.Watch("refs/heads/main");
}

public void Call_WatchExpression() {
	g_repo.Watch("main~1");
}

public void Call_WatchNegativeInterval() {
	g_repo.Watch("main", -1);
}

public void Call_ListWithoutCallback() {
	Git_ListRepositories(view_as<GitRepoListCallback>(INVALID_FUNCTION));
}

public void Test_ResultGettersRefuseBadIndexesAndKinds() {
	GitRepo repo = OpenRepo();
	repo.Log("v1.0.0", OnLog);
	Await();
	g_list = g_result;
	ExpectNativeError(CommitBeyondTheEnd, "Index 1 is outside the result's 1 entries");
	ExpectNativeError(CommitsAsATree, "is a git result of another kind");
	ExpectNativeError(CommitsAsRefs, "is a git result of another kind");
	delete g_list;

	g_repo = repo;
	g_list = null;
	ExpectNativeError(RepoAsATree, "Invalid git result handle");
	delete repo;
}

void CommitBeyondTheEnd() {
	view_as<GitCommitList>(g_list).GetTime(1);
}

void CommitsAsATree() {
	view_as<GitTree>(g_list).GetSize(0);
}

void CommitsAsRefs() {
	view_as<GitRefList>(g_list).IsTag(0);
}

void RepoAsATree() {
	view_as<GitTree>(g_repo).GetSize(0);
}

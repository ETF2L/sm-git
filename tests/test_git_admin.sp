/**
 * Copyright 2026 ETF2L Contributors, Dave
 *
 * The tests of the git-admin plugin (scripting/git-admin.sp) and its sm_git
 * command, run with smrunner (sm-runner) against the built extension and
 * sm-git's own repository on GitHub:
 *
 *     smrunner test test_git_admin.smx --extensions <dist>/extensions \
 *         --plugin git-admin.smx \
 *         --file <dist>/configs/git/ca-bundle.crt=addons/sourcemod/configs/git/ca-bundle.crt
 *
 * The commands answer a moment later, from the extension's callbacks; the
 * tests wait for the answer in the server console.
 */
#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <smrunner>

#define URL  "https://github.com/ETF2L/sm-git"
#define NAME "github.com/ETF2L/sm-git"

/* ------------------------------------------------------------------ helpers */

/* The answers waited for; WaitUntil gets the index of one. */
ArrayList g_waitedFor;

bool HasPrinted(any text) {
	char wanted[256];
	g_waitedFor.GetString(text, wanted, sizeof(wanted));
	return Runner.ConsoleContains(wanted);
}

/* Runs a server command and waits until the console printed answer. */
void CommandAnswers(const char[] command, const char[] answer) {
	if (g_waitedFor == null) {
		g_waitedFor = new ArrayList(ByteCountToCells(256));
	}
	int index = g_waitedFor.PushString(answer);
	Runner.Command("%s", command);
	Assert.True(Runner.WaitUntil(HasPrinted, 60.0, index), "%s answered \"%s\"", command, answer);
}

void CloneRepository() {
	CommandAnswers("sm_git clone \"" ... URL ... "\"", NAME ... ": cloned (main ");
}

/* ----------------------------------------------------------------- commands */

public void Test_CloneClonesOnceAndTellsTheHead() {
	CloneRepository();
	CommandAnswers("sm_git clone \"" ... URL ... "\"", NAME ... ": already there (main ");
}

public void Test_ListShowsTheClonedRepositories() {
	CloneRepository();
	CommandAnswers("sm_git list", NAME ... "   main ");
	Assert.ConsoleContains("fetched ", "with the time of its last fetch");
}

public void Test_StatusShowsTheRepositoryWithItsRefs() {
	CloneRepository();
	CommandAnswers("sm_git status sm-git", "  url       " ... URL);
	Assert.ConsoleContains("  head      main ");
	Assert.ConsoleContains("  branches  ", "with its branches");
	Assert.ConsoleContains("  tags      ", "and its tags");
	Assert.ConsoleContains("v1.0.0 65c9346", "the tag with its short SHA");
}

public void Test_LogShowsTheCommitsOfARevision() {
	CloneRepository();
	CommandAnswers("sm_git log sm-git v1.0.0", "65c9346 ");
	Assert.ConsoleContains(", davenonymous: sm-git: Initial commit");
}

public void Test_LogRefusesACountOutsideItsRange() {
	CommandAnswers("sm_git log sm-git HEAD 51", "sm_git log: rev must not be empty and count must be 1 to 50");
}

public void Test_FetchTellsWhetherRefsChanged() {
	CloneRepository();
	CommandAnswers("sm_git fetch sm-git", NAME ... ": fetched, nothing new");
}

public void Test_RemoveDeletesTheRepository() {
	CloneRepository();
	CommandAnswers("sm_git remove sm-git", "No repository is named \"sm-git\"; remove takes the exact name or URL.");
	CommandAnswers("sm_git remove \"" ... NAME ... "\"", NAME ... ": removed");
	CommandAnswers("sm_git status sm-git", "No repository matches \"sm-git\".");
}

public void Test_UnknownRepositoriesAreTold() {
	CommandAnswers("sm_git status nothing-like-it", "No repository matches \"nothing-like-it\".");
}

public void Test_AnUnquotedUrlGetsAHint() {
	CommandAnswers("sm_git clone https://github.com/ETF2L/sm-git",
	               "sm_git clone: put the URL in quotes, sm_git clone \"https://...\"");
}

public void Test_WrongArgumentsGetTheUsage() {
	CommandAnswers("sm_git", "Usage: sm_git list");
	Assert.ConsoleContains("       sm_git remove <name|url>");
	CommandAnswers("sm_git status", "Usage: sm_git list");
}

public void Test_TheCommandNeedsTheRootFlag() {
	int player = Runner.ConnectClient("player");
	SetUserFlagBits(player, ADMFLAG_GENERIC | ADMFLAG_CONFIG);
	Runner.ClientCommand(player, "sm_git list");
	Assert.ClientReceived(player, RunnerMessage_Console, "You do not have access to this command");

	int root = Runner.ConnectClient("root");
	SetUserFlagBits(root, ADMFLAG_ROOT);
	Runner.ClientCommand(root, "sm_git");
	Assert.ClientReceived(root, RunnerMessage_Console, "Usage: sm_git list");
}

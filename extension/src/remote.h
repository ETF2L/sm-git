/* Copyright 2026 ETF2L Contributors, Dave
 *
 * What plugins name things with, parsed once at the boundary: the remote of
 * a repository and paths inside a repository. Everything behind the parsers
 * only sees values that passed them. */
#ifndef SMGIT_REMOTE_H
#define SMGIT_REMOTE_H

#include <optional>
#include <string>

namespace smgit {

/* A repository on an HTTPS git server. */
struct Remote {
	std::string url;  /* normalized: https://<host>/<path>, without ".git" */
	std::string name; /* <host>/<path>, also its directory below the root, plus ".git" */
};

/* Accepts only https:// URLs of a host (optionally with a port) and a path
 * of plain segments: no user info, query, fragment, "." or "..", no
 * segment ending in ".git" but the last, and no last segment of only
 * ".git". A trailing slash and ".git" are dropped and the host is
 * lower-cased. On refusal the reason is in error. */
std::optional<Remote> ParseRemoteUrl(const std::string &url, std::string &error);

/* A path inside a repository: relative, segments separated by single
 * slashes, each of them a safe entry name (see IsSafeEntryName). A trailing
 * slash is dropped; the empty string is the repository's top directory. */
std::optional<std::string> ParseRepoPath(const std::string &path);

/* A path below the game directory: like a path inside a repository, but
 * never empty and without backslashes, which SourceMod's path functions
 * would turn into slashes. */
std::optional<std::string> ParseGamePath(const std::string &path);

/* directory + "/" + relative; none when that would not stay at or below
 * directory once "." and ".." are resolved. */
std::optional<std::string> PathBelow(const std::string &directory, const std::string &relative);

/* Whether path names directory or a place below it, comparing the two as
 * written after resolving "." and ".." (no symlinks are followed). */
bool IsAtOrBelow(const std::string &path, const std::string &directory);

/* Whether a single name of a tree entry is safe to create on disk: not
 * empty, ".", ".." or ".git" (in any case, which would plant a repository),
 * and without "/" or NUL. */
bool IsSafeEntryName(const std::string &name);

} // namespace smgit

#endif // SMGIT_REMOTE_H

/* Copyright 2026 ETF2L Contributors, Dave */
#include "remote.h"

#include <ctype.h>
#include <strings.h>

#include <filesystem>

namespace smgit {

namespace {

namespace fs = std::filesystem;

const char kScheme[] = "https://";
const char kGitDirectoryName[] = ".git";
const size_t kSchemeLength = sizeof(kScheme) - 1;

bool IsHostChar(char c)
{
	return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
}

bool IsPathChar(char c)
{
	return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '~';
}

bool EndsWith(const std::string &text, const std::string &suffix)
{
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* The path with "." and ".." resolved and without a trailing slash. */
std::string NormalizedPath(const std::string &path)
{
	std::string normal = fs::path(path).lexically_normal().string();
	while (normal.size() > 1 && normal.back() == '/')
		normal.pop_back();
	return normal;
}

/* "host" or "host:port", lower-cased; empty when it is neither. */
std::string ParseAuthority(const std::string &authority)
{
	size_t colon = authority.find(':');
	std::string host = authority.substr(0, colon);
	if (host.empty() || host.front() == '.' || host.front() == '-')
		return {};
	for (char &c : host) {
		if (!IsHostChar(c))
			return {};
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	}
	if (colon == std::string::npos)
		return host;

	std::string port = authority.substr(colon + 1);
	if (port.empty() || port.size() > 5)
		return {};
	for (char c : port) {
		if (!isdigit(static_cast<unsigned char>(c)))
			return {};
	}
	return host + ":" + port;
}

} // namespace

bool IsSafeEntryName(const std::string &name)
{
	if (name.empty() || name == "." || name == ".." || strcasecmp(name.c_str(), kGitDirectoryName) == 0)
		return false;
	return name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

bool IsAtOrBelow(const std::string &path, const std::string &directory)
{
	std::string place = NormalizedPath(path);
	std::string bound = NormalizedPath(directory);
	if (bound.empty() || place.compare(0, bound.size(), bound) != 0)
		return false;
	return place.size() == bound.size() || bound == "/" || place[bound.size()] == '/';
}

std::optional<std::string> PathBelow(const std::string &directory, const std::string &relative)
{
	std::string joined = directory + "/" + relative;
	if (!IsAtOrBelow(joined, directory))
		return std::nullopt;
	return joined;
}

std::optional<Remote> ParseRemoteUrl(const std::string &url, std::string &error)
{
	if (url.compare(0, kSchemeLength, kScheme) != 0) {
		error = "only https:// URLs are accepted";
		return std::nullopt;
	}
	size_t pathStart = url.find('/', kSchemeLength);
	if (pathStart == std::string::npos) {
		error = "the URL names no repository path";
		return std::nullopt;
	}
	std::string authority = ParseAuthority(url.substr(kSchemeLength, pathStart - kSchemeLength));
	if (authority.empty() || EndsWith(authority.substr(0, authority.find(':')), ".git")) {
		error = "the URL's host must be a plain host name, optionally with a port";
		return std::nullopt;
	}

	std::string path = url.substr(pathStart + 1);
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	/* Without its ".git", a last part of only ".git" would name another
	 * remote: "owner/.git" would become "owner". */
	if (path == kGitDirectoryName || EndsWith(path, std::string("/") + kGitDirectoryName)) {
		error = "the URL's path must name a repository, not end in a part \".git\"";
		return std::nullopt;
	}
	if (EndsWith(path, kGitDirectoryName))
		path.erase(path.size() - (sizeof(kGitDirectoryName) - 1));
	for (char c : path) {
		if (c != '/' && !IsPathChar(c)) {
			error = "the URL's path may only hold letters, digits, '.', '-', '_', '~' and '/'";
			return std::nullopt;
		}
	}
	std::optional<std::string> repoPath = ParseRepoPath(path);
	if (!repoPath || repoPath->empty()) {
		error = "the URL's path must name a repository, without empty, \".\" or \"..\" segments";
		return std::nullopt;
	}

	/* A part ending in ".git" would put this repository inside the directory
	 * of another one. */
	if (EndsWith(*repoPath, ".git") || repoPath->find(".git/") != std::string::npos) {
		error = "no part of the URL's path may end in \".git\", except the last";
		return std::nullopt;
	}

	std::string name = authority + "/" + *repoPath;
	return Remote{kScheme + name, name};
}

std::optional<std::string> ParseGamePath(const std::string &path)
{
	if (path.find('\\') != std::string::npos)
		return std::nullopt;
	std::optional<std::string> parsed = ParseRepoPath(path);
	if (!parsed || parsed->empty())
		return std::nullopt;
	return parsed;
}

std::optional<std::string> ParseRepoPath(const std::string &path)
{
	std::string trimmed = path;
	if (!trimmed.empty() && trimmed.back() == '/')
		trimmed.pop_back();
	if (trimmed.empty())
		return trimmed;

	for (size_t start = 0; ; ) {
		size_t end = trimmed.find('/', start);
		std::string segment = trimmed.substr(start, end == std::string::npos ? end : end - start);
		if (!IsSafeEntryName(segment))
			return std::nullopt;
		if (end == std::string::npos)
			return trimmed;
		start = end + 1;
	}
}

} // namespace smgit

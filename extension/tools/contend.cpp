/* Copyright 2026 ETF2L Contributors, Dave
 *
 * git-contend: opens (cloning when missing) and fetches one repository
 * below a root, as the extension does, and prints its default branch's
 * commit. Several started at once on one root show that servers sharing
 * a root do not get in each other's way.
 *
 * Usage: git-contend <CA bundle> <root> <url> */
#include "repo.h"

#include <stdio.h>

using namespace smgit;

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <CA bundle> <root> <url>\n", argv[0]);
		return 2;
	}
	Config config;
	std::string failure = Initialize(config, argv[1]);
	std::optional<Remote> remote = failure.empty() ? ParseRemoteUrl(argv[3], failure) : std::nullopt;
	if (!remote) {
		fprintf(stderr, "%s\n", failure.c_str());
		return 2;
	}

	std::atomic<bool> abort{false};
	std::string path = RepositoryPath(argv[2], *remote);
	if (!Exists(path)) {
		Result<Nothing> cloned = Clone(path, *remote, config.maxRepositoryBytes, abort);
		if (!cloned.ok()) {
			fprintf(stderr, "clone: %s\n", cloned.error.c_str());
			return 1;
		}
	}
	Result<FetchReport> fetched = Fetch(path, config.maxRepositoryBytes, abort);
	if (!fetched.ok()) {
		fprintf(stderr, "fetch: %s\n", fetched.error.c_str());
		return 1;
	}
	printf("%s\n", fetched.value.refs.headSha.c_str());
	Shutdown();
	return 0;
}

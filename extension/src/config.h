/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The settings of addons/sourcemod/configs/git.cfg. */
#ifndef SMGIT_CONFIG_H
#define SMGIT_CONFIG_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace smgit {

const int kMinWatchInterval = 60;
/* The longest connect_timeout and stall_timeout, in seconds; libgit2 takes
 * them in milliseconds as an int. */
const int kMaxTimeout = 3600;

struct Config {
	std::string root;           /* "root": where the repositories are kept; empty: the default */
	std::string caFile;         /* "ca_file": CA certificates trusted besides the shipped bundle */
	int watchInterval = 300;    /* "watch_interval": seconds between the polls of a watch */
	int connectTimeout = 10;    /* "connect_timeout": seconds to reach a remote */
	int stallTimeout = 30;      /* "stall_timeout": seconds a remote may send nothing */
	uint64_t maxRepositoryBytes = 200ull * 1024 * 1024; /* "max_size_mb" */
};

/* Builds the settings from the file's keys and values. An unknown key, a
 * value that is no positive number where one is needed and a timeout above
 * kMaxTimeout are errors. */
std::optional<Config> ParseConfig(const std::map<std::string, std::string> &values, std::string &error);

} // namespace smgit

#endif // SMGIT_CONFIG_H

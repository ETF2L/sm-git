/* Copyright 2026 ETF2L Contributors, Dave */
#include "config.h"

#include <stdlib.h>

namespace smgit {

namespace {

/* A whole positive decimal number of at most nine digits. */
std::optional<int> ParsePositive(const std::string &text)
{
	if (text.empty() || text.size() > 9)
		return std::nullopt;
	for (char c : text) {
		if (c < '0' || c > '9')
			return std::nullopt;
	}
	int value = atoi(text.c_str());
	if (value <= 0)
		return std::nullopt;
	return value;
}

} // namespace

std::optional<Config> ParseConfig(const std::map<std::string, std::string> &values, std::string &error)
{
	Config config;
	for (const auto &[key, value] : values) {
		if (key == "root") {
			config.root = value;
			continue;
		}
		if (key == "ca_file") {
			config.caFile = value;
			continue;
		}

		std::optional<int> number = ParsePositive(value);
		if (key != "watch_interval" && key != "connect_timeout" && key != "stall_timeout" && key != "max_size_mb") {
			error = "unknown setting \"" + key + "\"";
			return std::nullopt;
		}
		if (!number) {
			error = "\"" + key + "\" must be a positive number, not \"" + value + "\"";
			return std::nullopt;
		}
		bool isTimeout = key == "connect_timeout" || key == "stall_timeout";
		if (isTimeout && *number > kMaxTimeout) {
			error = "\"" + key + "\" must be at most " + std::to_string(kMaxTimeout) + " seconds, not \"" + value + "\"";
			return std::nullopt;
		}
		if (key == "watch_interval")
			config.watchInterval = *number < kMinWatchInterval ? kMinWatchInterval : *number;
		else if (key == "connect_timeout")
			config.connectTimeout = *number;
		else if (key == "stall_timeout")
			config.stallTimeout = *number;
		else
			config.maxRepositoryBytes = static_cast<uint64_t>(*number) * 1024 * 1024;
	}
	return config;
}

} // namespace smgit

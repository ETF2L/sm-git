/* Copyright 2026 ETF2L Contributors, Dave
 *
 * How an operation ended, and what it gave. The values of Status are the
 * GitStatus enum of git.inc. */
#ifndef SMGIT_STATUS_H
#define SMGIT_STATUS_H

#include <string>
#include <utility>

namespace smgit {

enum class Status {
	OK = 0,
	NotCloned, /* the repository is not (or no longer) on disk */
	NotFound,  /* no such revision, path or file */
	Refused,   /* a URL or path the extension does not accept */
	Network,   /* the remote could not be reached or answered wrongly */
	Timeout,   /* the remote took too long */
	TooLarge,  /* beyond the repository size cap or the read size cap */
	InUse,     /* a plugin has the repository open or watches it */
	Failed,    /* anything else; the error text tells */
};

/* The outcome of an operation: its value when status is OK, otherwise the
 * reason in error. */
template <typename T>
struct Result {
	Status status = Status::OK;
	std::string error;
	T value{};

	bool ok() const { return status == Status::OK; }
};

struct Nothing {};

template <typename T>
Result<T> Fail(Status status, std::string error)
{
	Result<T> result;
	result.status = status;
	result.error = std::move(error);
	return result;
}

template <typename T>
Result<T> Ok(T value)
{
	Result<T> result;
	result.value = std::move(value);
	return result;
}

/* Carries a failure over to a result of another type. */
template <typename T, typename U>
Result<T> FailLike(const Result<U> &failed)
{
	return Fail<T>(failed.status, failed.error);
}

} // namespace smgit

#endif // SMGIT_STATUS_H

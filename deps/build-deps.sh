#!/bin/bash
# Copyright 2026 ETF2L Contributors, Dave
#
# Builds the static libraries the git extension links, Mbed TLS and libgit2,
# for one target architecture into <prefix>/{include,lib}, from the sources
# fetch.py unpacked into <sources>. Position independent and with hidden
# visibility, since they end up inside a shared library loaded into srcds.
# Needs clang, make and CMake on the PATH.
#
# Usage: build-deps.sh <x86|x86_64> <sources> <prefix>
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: build-deps.sh <x86|x86_64> <sources> <prefix>" >&2
  exit 1
fi
arch="$1"
sources="$(cd "$2" && pwd)"

case "${arch}" in
  x86)    march="-m32 -march=pentium4 -msse2 -mfpmath=sse" ;;
  x86_64) march="-m64" ;;
  *)      echo "unknown architecture: ${arch}" >&2; exit 1 ;;
esac
prefix="$(mkdir -p "$3" && cd "$3" && pwd)"

cflags="${march} -O2 -fPIC -fvisibility=hidden -fno-omit-frame-pointer -D_FILE_OFFSET_BITS=64"
jobs="$(nproc)"
mkdir -p "${prefix}/include" "${prefix}/lib"

# Mbed TLS: the library directory's Makefile builds the three archives
work="$(mktemp -d)"
cp -a "${sources}/mbedtls/." "${work}/"
make -C "${work}/library" -j"${jobs}" CC=clang AR=ar CFLAGS="${cflags}" \
  libmbedcrypto.a libmbedx509.a libmbedtls.a
cp "${work}"/library/libmbed{crypto,x509,tls}.a "${prefix}/lib/"
cp -r "${work}/include/mbedtls" "${work}/include/psa" "${prefix}/include/"
rm -rf "${work}"

# libgit2: HTTPS through Mbed TLS only, with its bundled zlib, regex and HTTP
# parser inside libgit2.a; no SSH, no authentication helpers, no programs
work="$(mktemp -d)"
cmake -S "${sources}/libgit2" -B "${work}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_C_FLAGS="${cflags}" \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_FIND_ROOT_PATH="${prefix}" \
  -DMBEDTLS_ROOT_DIR="${prefix}" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF -DBUILD_EXAMPLES=OFF \
  -DUSE_HTTPS=mbedTLS -DUSE_SSH=OFF -DUSE_NTLMCLIENT=OFF -DUSE_GSSAPI=OFF \
  -DUSE_BUNDLED_ZLIB=ON -DREGEX_BACKEND=builtin -DUSE_HTTP_PARSER=builtin \
  -DUSE_ICONV=OFF -DUSE_NSEC=ON -DUSE_THREADS=ON
cmake --build "${work}" -j"${jobs}"
cmake --install "${work}"
rm -rf "${work}" "${prefix}/lib/pkgconfig" "${prefix}/lib/cmake"

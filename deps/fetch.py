#!/usr/bin/env python3
# Copyright 2026 ETF2L Contributors, Dave
#
# Downloads the pinned sources and tools of deps.json, checks each against its
# SHA-256 and unpacks it into <dir>: mbedtls/, libgit2/ and cmake/ (a
# release for an x86_64 build host) with their top directory stripped, and
# ca-bundle.crt (Mozilla's CA certificates as curl publishes them). Every
# build fetches through it, so the pins live in one place.
#
# Usage: fetch.py <dir>
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request

PINS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'deps.json')
ARCHIVES = ('mbedtls', 'libgit2', 'cmake')
CA_BUNDLE = 'ca-bundle'


def download_verified(url, expected_sha256, destination):
  with urllib.request.urlopen(url) as response, open(destination, 'wb') as out:
    shutil.copyfileobj(response, out)
  digest = hashlib.sha256()
  with open(destination, 'rb') as downloaded:
    for chunk in iter(lambda: downloaded.read(1 << 20), b''):
      digest.update(chunk)
  actual = digest.hexdigest()
  if actual != expected_sha256:
    raise SystemExit(f'{url}: SHA-256 is {actual}, deps.json pins {expected_sha256}')


def unpack_stripped(archive, target):
  unpacked = tempfile.mkdtemp(dir=os.path.dirname(archive))
  with tarfile.open(archive) as tar:
    tar.extractall(unpacked, filter='tar')
  top = os.listdir(unpacked)
  if len(top) != 1:
    raise SystemExit(f'{archive}: expected one top directory, found {len(top)}')
  if os.path.exists(target):
    shutil.rmtree(target)
  shutil.move(os.path.join(unpacked, top[0]), target)


def main():
  if len(sys.argv) != 2:
    raise SystemExit('usage: fetch.py <dir>')
  target = sys.argv[1]
  with open(PINS_FILE) as pins_file:
    pins = json.load(pins_file)
  missing = [name for name in (*ARCHIVES, CA_BUNDLE) if name not in pins]
  if missing:
    raise SystemExit(f'deps.json lacks {", ".join(missing)}')

  os.makedirs(target, exist_ok=True)
  download_verified(pins[CA_BUNDLE]['url'], pins[CA_BUNDLE]['sha256'], os.path.join(target, 'ca-bundle.crt'))
  with tempfile.TemporaryDirectory() as scratch:
    for name in ARCHIVES:
      archive = os.path.join(scratch, name)
      download_verified(pins[name]['url'], pins[name]['sha256'], archive)
      unpack_stripped(archive, os.path.join(target, name))
      print(f'{name}: {pins[name]["url"]}')


if __name__ == '__main__':
  main()

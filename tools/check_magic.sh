#!/bin/sh
set -eu

file=${1:?usage: check_magic.sh cv_dl_magic.bin}
expected_size=128
expected_sha=2db04d67f42015246ea037e640337569fc5585eb2fcf5dd59396f4ddcaf3a812

size=$(wc -c < "$file" | tr -d '[:space:]')
if [ "$size" != "$expected_size" ]; then
  echo "cv_dl_magic.bin size mismatch: got $size, expected $expected_size" >&2
  exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$file" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
  actual=$(shasum -a 256 "$file" | awk '{print $1}')
else
  echo "need sha256sum or shasum to verify cv_dl_magic.bin" >&2
  exit 1
fi

if [ "$actual" != "$expected_sha" ]; then
  echo "cv_dl_magic.bin sha256 mismatch:" >&2
  echo "  got      $actual" >&2
  echo "  expected $expected_sha" >&2
  exit 1
fi

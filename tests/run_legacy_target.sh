#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp)
trap 'rm -f "$test_binary"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -I"$repo_dir/main/utils/inc" "$repo_dir/tests/legacy_target_test.cpp" -o "$test_binary"
"$test_binary"

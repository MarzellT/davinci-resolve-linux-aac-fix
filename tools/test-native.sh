#!/usr/bin/env bash
# Pure parser and fail-closed constructor tests; never launches Resolve.
set -euo pipefail
port_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations -fno-omit-frame-pointer)
gcc "${flags[@]}" -fsanitize=address,undefined \
 "$port_dir/tools/test-native-parser.c" "$port_dir/src/aac_native_21.c" \
 "$port_dir/src/aac_native_21.S" -ldl -lcrypto -pthread -o "$test_dir/parser"
"$test_dir/parser"
gcc "${flags[@]}" "$port_dir/tools/test-version-guard.c" \
 "$port_dir/src/aac_native_21.S" -ldl -lcrypto -pthread -o "$test_dir/version-guard"
"$test_dir/version-guard"

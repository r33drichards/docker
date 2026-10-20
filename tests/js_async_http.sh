#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -Imodules)
read -r -a curl_cflags <<< "$(pkg-config --cflags libcurl)"
read -r -a curl_libs <<< "$(pkg-config --libs libcurl)"
"${CXX:-c++}" "${flags[@]}" "${curl_cflags[@]}" tests/js_async_http.cpp "${curl_libs[@]}" -o "$build/policy"
"$build/policy"
# Separate fixture-only executable. This flag is never used in production builds.
"${CXX:-c++}" "${flags[@]}" "${curl_cflags[@]}" -DJS_ASYNC_HTTP_TEST_LOOPBACK tests/js_async_http.cpp "${curl_libs[@]}" -o "$build/fixture"
"$build/fixture"

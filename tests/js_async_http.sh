#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
# pkg-config emits compiler arguments, intentionally split into separate words.
# shellcheck disable=SC2046
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -Imodules $(pkg-config --cflags libcurl) tests/js_async_http.cpp $(pkg-config --libs libcurl) -o "$build/policy"
"$build/policy"
# Separate fixture-only executable. This flag is never used in production builds.
# shellcheck disable=SC2046
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -Imodules $(pkg-config --cflags libcurl) -DJS_ASYNC_HTTP_TEST_LOOPBACK tests/js_async_http.cpp $(pkg-config --libs libcurl) -o "$build/fixture"
"$build/fixture"

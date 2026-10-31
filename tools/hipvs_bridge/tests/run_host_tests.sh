#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${1:?Usage: run_host_tests.sh /absolute/path/to/build-dir}
mkdir -p "$BUILD_DIR"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pthread -I"$SCRIPT_DIR/include" \
  -c "$SCRIPT_DIR/../seekdb_cuvs_bridge.c" -o "$BUILD_DIR/bridge-c.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -I"$SCRIPT_DIR/include" \
  "$BUILD_DIR/bridge-c.o" "$SCRIPT_DIR/test_search_workspace.cpp" -o "$BUILD_DIR/test-search-c"
"$BUILD_DIR/test-search-c"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -x c++ -I"$SCRIPT_DIR/include" \
  "$SCRIPT_DIR/../seekdb_cuvs_bridge.c" "$SCRIPT_DIR/test_search_workspace.cpp" -o "$BUILD_DIR/test-search-cpp"
"$BUILD_DIR/test-search-cpp"

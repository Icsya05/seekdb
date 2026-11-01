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

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -I"$SCRIPT_DIR/include" \
  "$BUILD_DIR/bridge-c.o" "$SCRIPT_DIR/test_persistence_host.cpp" -o "$BUILD_DIR/test-smoke-host"
expect_status() {
  local expected=$1 actual=0
  shift
  "$@" || actual=$?
  [[ "$actual" -eq "$expected" ]] || { echo "expected exit $expected, got $actual" >&2; return 1; }
}
expect_status 0 "$BUILD_DIR/test-smoke-host" write host-fixture
expect_status 3 "$BUILD_DIR/test-smoke-host" write host-fixture serialize-failure
expect_status 5 "$BUILD_DIR/test-smoke-host" write host-fixture copy-failure
expect_status 2 "$BUILD_DIR/test-smoke-host" typo host-fixture
printf '%s\n' 'PASS smoke host control-flow and error cleanup (no GPU persistence claim)'

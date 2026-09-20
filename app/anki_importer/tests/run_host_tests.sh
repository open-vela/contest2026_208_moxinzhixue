#!/bin/sh

set -eu

if [ "$#" -gt 0 ]; then
  OPENVELA_WORKSPACE=$1
fi

: "${OPENVELA_WORKSPACE:?set OPENVELA_WORKSPACE or pass the workspace path}"

TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_ROOT=${ANKI_TEST_BUILD_ROOT:-/tmp/moxinzhi-anki-host-tests}

cmake -S "$TEST_ROOT" -B "$BUILD_ROOT" \
  -DOPENVELA_WORKSPACE="$OPENVELA_WORKSPACE" \
  ${ANKI_TEST_CMAKE_ARGS:-}
cmake --build "$BUILD_ROOT" --parallel
ctest --test-dir "$BUILD_ROOT" --output-on-failure

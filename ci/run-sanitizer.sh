#!/usr/bin/env bash
# Build the unit tests under one sanitizer and run them. Any finding fails.
set -euo pipefail
usage='Usage: run-sanitizer.sh address|undefined|thread|leak|memory [build-directory]'
sanitizer=${1:?$usage}
build=${2:-build}
source=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exclude=()
case "$sanitizer" in
  # test/CMakeLists.txt registers these flavours through cmake/dtest.cmake.
  address) flavour=asan; options=(-DBUILD_ASAN_TESTS=On -DBUILD_ONLY_SANITIZED=On) ;;
  undefined) flavour=ubsan; options=(-DBUILD_UBSAN_TESTS=On -DBUILD_ONLY_SANITIZED=On) ;;
  thread) flavour=tsan; options=(-DBUILD_TSAN_TESTS=On -DBUILD_ONLY_SANITIZED=On) ;;
  # The remaining modes instrument the plain flavour and GoogleTest with global
  # flags, so they skip the suite that dtest registers as NO_SANITIZE.
  leak)
    flavour=nosan
    exclude=(-E '^TestUninitializedMemory\.')
    options=('-DCMAKE_CXX_FLAGS=-fsanitize=leak -fno-omit-frame-pointer')
    ;;
  memory)
    : "${MSAN_LIBCXX_ROOT:?Set MSAN_LIBCXX_ROOT to a MemorySanitizer-instrumented libc++ installation}"
    if [[ ! -d "$MSAN_LIBCXX_ROOT/include/c++/v1" || ! -d "$MSAN_LIBCXX_ROOT/lib" ]]; then
      echo "No libc++ installation in MSAN_LIBCXX_ROOT=$MSAN_LIBCXX_ROOT" >&2
      exit 1
    fi
    flavour=nosan
    exclude=(-E '^TestUninitializedMemory\.')
    options=(
      "-DCMAKE_CXX_FLAGS=-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-sanitize-recover=all -fno-omit-frame-pointer -stdlib=libc++ -nostdinc++ -isystem $MSAN_LIBCXX_ROOT/include/c++/v1"
      "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -L$MSAN_LIBCXX_ROOT/lib -Wl,-rpath,$MSAN_LIBCXX_ROOT/lib"
      # Not the tests' usual -Og. With Clang 21, a test compiled at -Og reports
      # a use after destruction in the stream GoogleTest writes to while its
      # globals are initialized, before main. The same code compiled at -O0,
      # -O1 or -O2 reports nothing, so it is the code generated at that level
      # and not the program. Every check stays on.
      -DDTEST_OPTIMIZATION=-O1
    )
    ;;
  *)
    echo "Unknown sanitizer: $sanitizer" >&2
    echo "$usage" >&2
    exit 2
    ;;
esac
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
cmake -S "$source" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DPARLAY_TEST=On -DCMAKE_CXX_STANDARD=23 "${options[@]}"
cmake --build "$build" --parallel "${SANITIZER_BUILD_JOBS:-4}"
# ctest rejects a -R value that starts with a dash.
ctest --test-dir "$build" --no-tests=error --output-on-failure -R "[-]$flavour\$" ${exclude[@]+"${exclude[@]}"}

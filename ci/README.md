# Continuous integration and packages

ParlayLib itself still requires only C++17. CI uses C++23 as its baseline and also
builds the unit tests, the examples and the consumer in C++26 mode with Clang; that
checks compiler support for this code, not completeness of the C++26 standard
library. One additional Clang build uses C++17 so the declared minimum stays checked.

The GCC build and the publication consumer use the
[official GCC 16.2.0 image](https://hub.docker.com/_/gcc), pinned by manifest digest.
The container setup checks the effective compiler version. Linux Clang builds use
Clang 18 on `ubuntu-24.04`. macOS uses the
[`xcode-27` ARM64 image](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md)
with [Xcode 27.2](https://developer.apple.com/xcode/system-requirements) explicitly
selected, rather than the image's default toolchain. Compiler versions are printed
in the logs.

The workflow runs on pull requests, `master`, release/hotfix branch pushes, and
manual requests. Every build job configures the project in Debug mode with
`PARLAY_TEST` and `PARLAY_EXAMPLES`, runs the GoogleTest suite registered in
`test/CMakeLists.txt` with `PARLAY_NUM_THREADS=16`, installs with CMake and builds
and runs the separate consumer in `test_package/` against that installation.
Conan 2.31.2 creates and consumes the header-only package on each C++23 platform.
`script-tests` runs the regression tests of the publication and sanitizer scripts.

This workflow replaces the former `build.yml` and `check-version.yml`. It does not
build the benchmarks, does not run Windows (MSVC, WSL), and does not exercise the
alternative schedulers (OpenCilk, TBB, sequential), the exceptions-disabled build,
Cppcheck, Clang-Tidy, include-what-you-use or coverage reporting; the CMake options
and targets for all of those remain in the project.

## Sanitizers

ASan, UBSan, TSan, LSan and MSan are independent required jobs using Clang 21, all
driven by `ci/run-sanitizer.sh <sanitizer> [build-directory]`:

- `address`, `undefined` and `thread` build the instrumented flavours that
  `cmake/dtest.cmake` already provides (`BUILD_ASAN_TESTS`, `BUILD_UBSAN_TESTS`,
  `BUILD_TSAN_TESTS` with `BUILD_ONLY_SANITIZED`) and run the `-asan`, `-ubsan` or
  `-tsan` tests. GoogleTest itself is not instrumented in these flavours.
- `leak` has no dtest flavour, and dtest's MSan flavour compiles against the
  system libc++ headers rather than those of the instrumented installation. Both
  modes therefore pass their flags through `CMAKE_CXX_FLAGS`, which instruments
  GoogleTest too, and run dtest's plain flavour: test names in these two jobs end
  in `-nosan` although the binaries are instrumented. They skip
  `TestUninitializedMemory`, which `test/CMakeLists.txt` registers as `NO_SANITIZE`
  and which the dtest flavours leave out as well.

MSan builds an instrumented [LLVM 21.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8)
[libc++](https://libcxx.llvm.org/)/[libc++abi](https://libcxxabi.llvm.org/) with
`ci/msan-libcxx.sh` and caches that installation. The runtime build disables its
own upstream tests and benchmarks. The script refuses to run `memory` without
`MSAN_LIBCXX_ROOT` pointing to such an installation.

No sanitizer findings are suppressed or allowed to fail; `.sanitizer-blacklist`
has no entries. Any sanitizer failure blocks publication.

On macOS the `address` flavour is not usable as is: libc++ annotates
`std::vector` storage in instrumented code only, so sharing vectors with the
uninstrumented GoogleTest library reports false overflows unless
`ASAN_OPTIONS=detect_container_overflow=0` is set. LeakSanitizer and
MemorySanitizer are not available with Apple Clang.

## Publishing

Only successful pushes to `release/X.Y.Z` or `hotfix/X.Y.Z` publish
`parlaylib/X.Y.Z`. `master`, PRs, tags and manual dispatches validate without
publishing. All validation jobs must pass.

The version has a single source: `project(PARLAY VERSION X.Y.Z ...)` in the
top-level `CMakeLists.txt`. The Conan recipe reads it from there in `set_version()`,
and `ci/publish.sh` refuses to publish when the branch names any other version, so
a release branch is cut after the project version has been set.

Set repository secrets `CONAN_LOGIN_USERNAME` and `CONAN_PASSWORD` to credentials
with upload permission at `https://packages.kth.cash/api/`. No credentials belong
in this checkout. `ci/publish.sh` starts with a clean Conan cache in CI, creates
and tests the package, uploads its exact recipe revision, then downloads that
revision and the header package into a new anonymous cache and runs the consumer
with `--build=never`. Both global and remote-specific credential environment
variables are removed before verification; network checks use
`core:non_interactive=True` so authentication cannot silently rescue a public
package download. The anonymous install retries up to 12 times, waiting five
seconds between failures, to allow the public server index to propagate. Every
attempt keeps the exact revision, anonymous environment and `--build=never`;
exhausting the attempts fails publication. Consumer execution is not retried.
`ci/test_publish.py` verifies that boundary, the bounded retries, the version
agreement and the rejection of non-release events without contacting the server.
A failed post-upload check fails the job; it does not remove an uploaded revision.

## Local use

```sh
conan profile detect
conan create . -pr:h ci/conan-cpp23 -pr:b default

cmake -S . -B build -DPARLAY_TEST=On
cmake --build build
ctest --test-dir build
cmake --install build --prefix /tmp/parlay-install
cmake -S test_package -B build-consumer -DCMAKE_PREFIX_PATH=/tmp/parlay-install
cmake --build build-consumer
./build-consumer/consumer

python3 ci/test_publish.py
python3 ci/test_run_sanitizer.py
python3 ci/test_installation.py
bash ci/run-sanitizer.sh thread build-tsan
```

The CMake package is unchanged: `find_package(Parlay CONFIG REQUIRED)` and the
target `Parlay::parlay`. The Conan package `parlaylib` generates the same file and
target names through CMakeDeps, requires C++17 or later, and contains only
`include/parlay/` and the license. It models the default configuration; the
scheduler and elastic-parallelism options of the CMake build are not recipe options.

Header installation honors `CMAKE_INSTALL_INCLUDEDIR`, like the exported target's
include path. `ci/test_installation.py` installs into a default and a custom
header directory that already holds another project's header, checks that the
installed `parlay/` tree equals `include/parlay/` and that the other header is
untouched, then relocates the prefix and builds and runs the consumer against it.
The Conan consumer checks that the package's include root contains only `parlay/`.

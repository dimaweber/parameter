# Testing

`parameter`'s test suite is GTest-based, lives in `tests/parameter.unittest.cxx`,
and currently builds as part of the parent (minesweeper) project's CMake
configuration - see "Standalone build" below for what's missing to build it
outside that project.

## Building and running (inside the parent project)

```
cmake --build build --target parameter_test
```

Then either run it directly:

```
./build/libs/parameter/tests/parameter_test
./build/libs/parameter/tests/parameter_test --gtest_filter='ParametersBitstream.RejectsCorruptedPayload'
```

or through CTest, alongside every other test in the parent project:

```
ctest --test-dir build -R ParametersBitstream --output-on-failure
```

Building the tests at all is gated by `PARAMETER_BUILD_TESTS`
(`libs/parameter/CMakeLists.txt`), which defaults to whatever the parent
project's `BUILD_TESTS` option is set to - pass `-DPARAMETER_BUILD_TESTS=OFF`
to configure the `parameter` library without its tests regardless of that.

## What's covered

- Round trips: signed integers across a wide range (including
  `int64_t`'s min/max), strings, booleans, a flat list, a list of strings, a
  list of lists, and a map nesting scalars, a list, and a nested map.
- Instance reuse is rejected: a second `store()`/a `load()` on an instance that
  already `store()`d fails rather than corrupting the buffer or parsing from
  the wrong offset.
- `wire_version` mismatch is rejected.
- Payload corruption (a single flipped bit inside an otherwise well-formed
  payload) is caught by the checksum, not by the value happening to fail to
  parse.
- `load()` against an oversized buffer whose bytes past the real payload are
  non-zero "garbage" (e.g. leftover from a prior `store()` into the same
  scratch buffer) only trusts the length this `store()` actually wrote, never
  the buffer's full physical size.

## Standalone build

`tests/CMakeLists.txt` currently links against whatever `GTest::gtest_main`
target the *parent* project's top-level `CMakeLists.txt` already located (via
`find_package`/`FetchContent`) - there is no `find_package(GTest)`/
`FetchContent` call inside `libs/parameter/` itself yet. Building this
directory as its own standalone top-level project (outside minesweeper) will
need that added to `libs/parameter/CMakeLists.txt` or `tests/CMakeLists.txt`
before `tests/` will configure on its own. The `parameter` library target
itself has no such gap - its only dependency (fmt) is already resolved via
`find_package()` inside `libs/parameter/CMakeLists.txt`.

# Testing

`parameter`'s test suite is GTest-based and lives in `tests/parameter.unittest.cxx`.

## Building and running

```
cmake -S . -B build
cmake --build build --target parameter_test
```

Then either run it directly:

```
./build/tests/parameter_test
./build/tests/parameter_test --gtest_filter='ParametersBitstream.RejectsCorruptedPayload'
```

or through CTest:

```
ctest --test-dir build --output-on-failure
```

Tests are enabled by default and require GTest. Pass `-DPARAMETER_BUILD_TESTS=OFF` to
configure the `parameter` library without its tests.

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


# Changelog

All notable changes to this project will be documented in this file. This library
currently lives inside, and is versioned alongside, the minesweeper repository - see
that project's own top-level `CHANGELOG.md` for the commit(s) that correspond to each
entry below, until this directory is extracted into its own repository.

## [Unreleased]

### Added

- Initial version, extracted from `src/server/plugins/api.hxx` (where it was
  originally defined directly inside the plugin ABI header) with no behavior
  change: `parameter_t`/`parameter_list_t`/`parameter_map_t`/`result_t` (a
  recursive scalar/array/map value type) and `parameter_bytestream_t` (its
  compact, tagged binary (de)serializer - `store()`/`load()`, versioned via
  `wire_version`, checksummed, never throwing past its own boundary). Stays
  header-only despite the move - see the note at the top of
  `include/parameter/parameter.hxx` for why that's a deliberate choice here,
  not an ABI necessity (a `parameter_t` never crosses the plugin/host
  boundary as a raw object, only as bytes).
- Own `CMakeLists.txt` (an `INTERFACE` library target `parameter` - include
  path plus the fmt dependency, nothing compiled), built via
  `add_subdirectory(libs/parameter)` from the parent project, and linked
  into `ms_server` plus both plugin `.so` targets so they don't need
  `src/server/plugins/api.hxx`'s old `include_directories(${CMAKE_SOURCE_DIR}/)`
  reach for it.
- GTest-based test suite (`tests/`), moved unchanged from
  `src/server/tests/parameters_bitstream.unittest.cxx`, registered with the
  parent project's CTest run; see [`tests.md`](tests.md) for what it covers.
- `readme.md`, `tests.md`, this changelog.

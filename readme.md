# parameter

A small, header-only C++ library providing `parameter_t` - a recursive value type
(string/integer/bool/array/map, any of which can nest inside the array/map
alternatives) - and `parameter_bytestream_t`, a compact binary (de)serializer for it.

```cpp
#include <parameter/parameter.hxx>

const parameter_t value = parameter_map_t {
    {"name",  "Minesweeper"                 },
    {"width", int64_t {30}                  },
    {"tags",  parameter_list_t {"game", "cli"}},
};

std::array<std::byte, 256> buffer;
parameter_bytestream_t     writer(buffer);
if ( const auto stored = writer.store(value); !stored ) {
  // stored.error() is a std::string
}

parameter_bytestream_t reader(buffer);
const auto             loaded = reader.load( );  // result_t<parameter_t>
// *loaded == value
```

## Why this exists

`parameter_t` began life inside this repository's plugin ABI header
(`src/server/plugins/api.hxx`), where it's still used to describe HTTP request
parameters and handler results that cross the host/plugin `dlopen()` boundary as
raw bytes. It turned out to be independently useful - a plain recursive
value/serialization type has no actual dependency on plugins, boards, or HTTP -
so it now lives here, and `plugins/api.hxx` just `#include`s this header and
uses its types unqualified, same as before.

## `parameter_t`

A `struct` deriving from `std::variant<std::string, int64_t, uint64_t, bool,
parameter_list_t, parameter_map_t>` (`parameter_list_t` = `std::vector<parameter_t>`,
`parameter_map_t` = `std::unordered_map<std::string, parameter_t>`) - so a
`parameter_t` can hold a scalar, or an array/map of `parameter_t`s, recursively,
including arrays of arrays and maps of maps.

## `parameter_bytestream_t`

Wraps a caller-owned `std::byte` buffer (or `std::span<std::byte>`) and offers
exactly two public entry points:

- `store(const parameter_t&) -> result_t<void>`
- `load() -> result_t<parameter_t>`

Each instance is single-use (one `store()` **or** one `load()`, never both, and
never twice) - reuse is rejected rather than silently corrupting the buffer or
parsing from the wrong offset. Neither call ever throws past its own boundary:
every internal failure (buffer overrun, malformed tag, a `wire_version`
mismatch, a checksum mismatch, or reuse) comes back as the error side of
`result_t`.

### Wire format

A 1-byte version, then a fixed-width 8-byte payload length and 4-byte CRC-32
(covering just the payload), then the payload itself: a compact, recursively
tagged encoding where each value is a 1-byte type tag followed by only as many
length/value bytes as it actually needs (e.g. the integer `1` costs 2 bytes
total, not a fixed 8). `parameter_bytestream_t::wire_version` is bumped whenever
this grammar changes in a way an old reader would misparse; `load()` checks it
explicitly and fails rather than guessing.

## Dependencies

fmt only (for error-message formatting) - nothing here depends on anything else
in whichever repository it's currently vendored inside (see the note at the top
of `include/parameter/parameter.hxx`), so this directory can be lifted into its
own project unmodified.

## Header-only, on purpose

Unlike this repository's other extracted library (`libs/envelope/`, a compiled
static library), this one stays header-only even though it's built as its own
`libs/parameter/` directory - see the note at the top of
`include/parameter/parameter.hxx` for why, in short: a `parameter_t` never
crosses the plugin ABI boundary as a raw object (only its serialized bytes do,
versioned by `wire_version`), so there's no ABI reason to compile it into a
linked library, and the code is small and call-site-dependent enough that
staying inline is a real (not just simpler) win over adding a `.cxx`.

## Building

Currently built as part of the parent project via `add_subdirectory(libs/parameter)`,
producing an `INTERFACE` library target named `parameter` (include path + fmt
dependency only - nothing to compile or link). See [`tests.md`](tests.md) for
how its own test suite builds and runs.

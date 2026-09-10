#pragma once

/// @file
/// This header, and everything under `libs/parameter/`, must never include
/// or link against anything from the rest of this repository (`src/**`) -
/// same rule as `libs/envelope/`, and for the same reason: it's meant to be
/// liftable into its own project unmodified.
///
/// Unlike envelope, this library stays **header-only** even after
/// extraction. `parameter_t`/`parameter_bytestream_t` sit at the plugin ABI
/// boundary (`src/server/plugins/api.hxx` `#include`s this header and
/// re-exposes these names unqualified), but - unlike `board_i`/`plugin_api_i`
/// and friends in that header, which cross as vtable calls and so do need
/// `addon_api_abi_tag()`'s compiler/stdlib fingerprint to catch a layout
/// mismatch - a `parameter_t` itself never crosses that boundary as a raw
/// C++ object: every call site serializes it through `parameter_bytestream_t`
/// first and only ever passes the resulting `std::byte*`/`size_t` across a
/// vtable call or C function pointer (see `plugin_api_i::simple_handler_t`/
/// `board_handler_t` and `resource_wire`/`handler_wire` in `plugins/api.hxx`).
/// So this type's in-memory layout genuinely doesn't need to match between
/// the host and a plugin `.so` - what does need to match is the wire
/// *format* `store()`/`load()` produce, which is exactly what
/// `parameter_bytestream_t::wire_version` guards explicitly (a skewed
/// reader fails loudly rather than misparsing).
///
/// Given that, this could technically be a linked (even shared) library
/// instead. It stays header-only anyway: one shared copy of the
/// serialization logic keeps both sides trivially in lockstep without
/// relying on `wire_version` ever being exercised in practice, and it avoids
/// adding a new linked build dependency to every plugin `.so` for something
/// this small - each `write()`/`read_*()` call is a handful of branches
/// operating on one already-in-hand buffer, exactly the kind of code that
/// benefits from being inlined at the call site rather than going through
/// a real call, so there's no compiled-library win being left on the table
/// here. So this directory intentionally has no `.cxx` and no compiled
/// target, only headers a consumer includes directly - see `CMakeLists.txt`.

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <variant>
#include <vector>

/// A parameter is either a scalar value, an array of parameters of the same
/// (recursive) type (arrays of arrays are allowed), or a map of named
/// parameters of the same (recursive) type (maps of maps/arrays, and vice
/// versa, are allowed too). This is implemented as a variant deriving struct
/// so that `std::vector<parameter_t>` / `std::unordered_map<std::string,
/// parameter_t>` can appear as alternatives of `parameter_t` itself (allowed
/// since C++17 relaxed the incomplete-type requirements for `std::vector`;
/// libstdc++'s node-based `std::unordered_map` supports incomplete mapped
/// types the same way in practice).
struct parameter_t : std::variant<std::string, int64_t, uint64_t, bool, std::vector<parameter_t>, std::unordered_map<std::string, parameter_t>> {
  using variant::variant;
};

using parameter_list_t = std::vector<parameter_t>;
using parameter_map_t  = std::unordered_map<std::string, parameter_t>;

template<typename T>
using result_t = std::expected<T, std::string>;

struct parameter_bytestream_t {
  /// @todo: zip/unzip and a stronger (e.g. sha256) signature don't belong
  ///        in this class - every consumer of this header (every plugin
  ///        .so as well as the host) can only ever depend on what's
  ///        already a hard requirement for everyone building against it,
  ///        and pulling in a compression/crypto library for that would be
  ///        a permanent dependency tax for a same-process boundary that
  ///        doesn't actually have an adversary or a bandwidth problem.
  ///        Those belong as wrappers around the raw store()/load() bytes
  ///        instead - compress/sign before store()'s bytes go wherever
  ///        they're going, decompress/verify before they reach load().
  /// @note: this type used to live directly inside the plugin ABI header
  ///        (`src/server/plugins/api.hxx`) and has since moved into its
  ///        own library here, since it's genuinely useful beyond that one
  ///        boundary. It stays header-only regardless (see the @file
  ///        comment above for why that's a build-simplicity/inlining
  ///        choice here, not an ABI necessity) - a .cxx remains an option
  ///        later (e.g. a table-based CRC, or true concurrent partial-read
  ///        support with an unwind/rewind stack - speculative for now
  ///        since nothing here has a concurrent producer/consumer on one
  ///        buffer yet), it just isn't worth it for code this small and
  ///        call-site-dependent.
  parameter_bytestream_t (std::byte* buffer, size_t buffer_size) : buffer_(buffer), buffer_size_(buffer_size) {
  }

  explicit parameter_bytestream_t (std::span<std::byte> buffer) : buffer_(buffer.data( )), buffer_size_(buffer.size( )) {
  }

  /// Bump when serialize()/deserialize()'s wire grammar - or store()/load()'s
  /// own header shape below - changes in a way that would make an old reader
  /// misparse a new writer's bytes (new type_tag values appended at the end
  /// are fine; anything else isn't). Written/checked once per buffer by
  /// store()/load(), outside the recursive grammar itself -
  /// serialize()/deserialize() stay unaware it exists. Bumped to 2 for the
  /// length+checksum header fields below.
  static constexpr uint8_t wire_version = 2;

  /// Bytes actually written/consumed so far - lets a caller that store()d
  /// into a scratch buffer larger than it needed find out how much of it
  /// is real payload.
  [[nodiscard]] size_t size ( ) const noexcept {
    return static_cast<size_t>(offset_);
  }

  /// @{
  /// Public entry points: store()/load() are the only way in or out of a
  /// buffer, and neither ever lets an exception reach the caller - every
  /// internal failure (buffer overrun, malformed tag, version mismatch,
  /// checksum mismatch, reuse of an already-used instance) comes back as
  /// the error side of result_t, so a caller on the other side of a dlopen
  /// boundary can't forget to handle it and doesn't need to know this type
  /// can throw at all internally. serialize()/deserialize() are the
  /// recursive core and are deliberately private - a caller only ever
  /// deals in whole, versioned buffers.
  ///
  /// The header written here - version, then a fixed-width payload length
  /// and checksum - is deliberately outside serialize()/deserialize()'s own
  /// recursive, compact (numberX-tagged) grammar: it's a single one-time
  /// field per buffer, not a value repeated per array/map element, so the
  /// few extra fixed-width bytes don't matter and a fixed width means
  /// load() can find it without first having to parse anything.
  [[nodiscard]] result_t<void> store (const parameter_t& param) {
    if ( offset_ != 0 ) {
      return std::unexpected(fmt::format("parameter_bytestream_t::store: instance already used (offset {} != 0); use a fresh instance per buffer", offset_));
    }
    try {
      put_byte(wire_version);

      // Length and checksum are only known once serialize() has actually
      // run, so reserve their slot now and backfill it once the payload
      // is written.
      check_capacity(sizeof(uint64_t) + sizeof(uint32_t));
      const size_t header_pos = offset_;
      offset_ += sizeof(uint64_t) + sizeof(uint32_t);

      const size_t payload_pos = offset_;
      serialize(param);
      const size_t payload_len = offset_ - payload_pos;

      as_ref<uint64_t>(buffer_ + header_pos)                      = static_cast<uint64_t>(payload_len);
      as_ref<uint32_t>(buffer_ + header_pos + sizeof(uint64_t)) = crc32(buffer_ + payload_pos, payload_len);
    } catch ( const std::exception& e ) {
      return std::unexpected(fmt::format("parameter_bytestream_t::store failed: {}", e.what( )));
    }
    return { };
  }

  [[nodiscard]] result_t<parameter_t> load ( ) {
    if ( offset_ != 0 ) {
      return std::unexpected(fmt::format("parameter_bytestream_t::load: instance already used (offset {} != 0); use a fresh instance per buffer", offset_));
    }
    try {
      const uint8_t version = read_byte( );
      if ( version != wire_version ) {
        return std::unexpected(fmt::format("parameter_bytestream_t::load: wire version mismatch (expected {}, got {})", wire_version, version));
      }

      check_capacity(sizeof(uint64_t) + sizeof(uint32_t));
      const uint64_t payload_len       = as_ref<uint64_t>(buffer_ + offset_);
      const uint32_t expected_checksum = as_ref<uint32_t>(buffer_ + offset_ + sizeof(uint64_t));
      offset_ += sizeof(uint64_t) + sizeof(uint32_t);

      // Bytes past the real payload are whatever was already in the
      // buffer before this store() wrote it (uninitialized, or leftover
      // from an earlier use) - buffer_size_ alone can't tell an
      // oversized buffer from a truncated one, only the length this same
      // store() wrote can. Narrowing buffer_size_ here makes every
      // check_capacity() call for the rest of this load() - including
      // the whole recursive deserialize() descent - fail if it would
      // read past the real payload, not just past the physical buffer.
      check_capacity(payload_len);
      buffer_size_ = offset_ + payload_len;

      if ( crc32(buffer_ + offset_, payload_len) != expected_checksum ) {
        return std::unexpected(fmt::format("parameter_bytestream_t::load: checksum mismatch ({} byte payload)", payload_len));
      }

      const size_t     payload_pos = offset_;
      const parameter_t result     = deserialize( );
      if ( static_cast<size_t>(offset_) != payload_pos + payload_len ) {
        return std::unexpected(fmt::format("parameter_bytestream_t::load: {} trailing byte(s) after parsing a well-formed payload", payload_pos + payload_len - static_cast<size_t>(offset_)));
      }
      return result;
    } catch ( const std::exception& e ) {
      return std::unexpected(fmt::format("parameter_bytestream_t::load failed: {}", e.what( )));
    }
  }
  /// @}

private:
  /// Internal-only: serialize()/deserialize()'s recursive descent unwinds
  /// through this on any failure. It never crosses store()/load() - both
  /// catch std::exception and convert to result_t's error side before
  /// returning.
  struct error : std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  enum type_tag : uint8_t {
    null,
    string1,
    string2,
    string3,
    string4,
    string5,
    string6,
    string7,
    string8,
    number1,
    number2,
    number3,
    number4,
    number5,
    number6,
    number7,
    number8,
    neg_number1,
    neg_number2,
    neg_number3,
    neg_number4,
    neg_number5,
    neg_number6,
    neg_number7,
    neg_number8,
    boolean,
    vector,
    map,
  };

  std::byte* buffer_;
  size_t     buffer_size_;
  off_t      offset_ {0};

  template<typename T>
  T* as_ptr (std::byte* ptr) {
    return std::bit_cast<T*>(ptr);
  }

  template<typename T>
  T* as_ptr ( ) {
    return as_ptr<T>(buffer_ + offset_);
  }

  template<typename T>
  T& as_ref (std::byte* ptr) {
    return *as_ptr<T>(ptr);
  }

  template<typename T>
  T& as_ref ( ) {
    return *as_ptr<T>(buffer_ + offset_);
  }

  /// Every read/write primitive below funnels through here first, so a
  /// truncated, corrupted, or maliciously short buffer fails loudly right
  /// where it would otherwise read/write out of bounds, instead of silently
  /// touching memory past buffer_size_.
  void check_capacity (size_t needed) const {
    if ( static_cast<size_t>(offset_) + needed > buffer_size_ ) {
      throw error(fmt::format("parameter_bytestream_t: buffer overrun at offset {} (need {} more bytes, capacity {})", offset_, needed, buffer_size_));
    }
  }

  /// How many bytes numberN/stringN needs to hold v - used up front so
  /// check_capacity() reserves exactly that much instead of always the
  /// 8-byte worst case, which is what write() actually used to check even
  /// though the whole point of numberN/stringN is that most values need
  /// far less.
  [[nodiscard]] static constexpr int bytes_needed (uint64_t v) noexcept {
    int n = 1;
    for ( ; n < 8 && v >= (uint64_t {1} << (n * 8)); ++n ) {
    }
    return n;
  }

  /// Plain bitwise CRC-32 (IEEE 802.3 / zlib polynomial) over the payload -
  /// no table, since payloads here are small enough (KB, not GB) that the
  /// per-byte cost is a non-issue, and a table would mean static
  /// initialization-order reasoning across every plugin that includes this
  /// header for no real benefit. This defends only against accidental
  /// corruption/truncation/version skew inside one host process - not
  /// against a tampering adversary, which is a different (and here,
  /// inapplicable) threat model.
  [[nodiscard]] static uint32_t crc32 (const std::byte* data, size_t len) noexcept {
    uint32_t crc = 0xFFFF'FFFFu;
    for ( size_t i = 0; i < len; ++i ) {
      crc ^= static_cast<uint8_t>(data[i]);
      for ( int bit = 0; bit < 8; ++bit ) {
        const uint32_t mask = -(crc & 1u);
        crc = (crc >> 1) ^ (0xEDB8'8320u & mask);
      }
    }
    return ~crc;
  }

  void write_tag (type_tag tag) {
    check_capacity(sizeof(type_tag));
    as_ref<type_tag>( ) = tag;
    offset_ += sizeof(type_tag);
  }

  void write_len (size_t len) {
    write(len);
  }

  template<std::convertible_to<std::string> T>
  void write (const T& str) {
    const size_t str_size = str.size( );
    check_capacity(sizeof(type_tag) + bytes_needed(str_size) + str_size);
    if ( str_size < 0x100 ) {
      write_tag(type_tag::string1);
      put_byte(byte(str_size, 0));
    } else if ( str_size < 0x1'00'00 ) {
      write_tag(type_tag::string2);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
    } else if ( str_size < 0x1'00'00'00 ) {
      write_tag(type_tag::string3);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
    } else if ( str_size < 0x1'00'00'00'00 ) {
      write_tag(type_tag::string4);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
      put_byte(byte(str_size, 3));
    } else if ( str_size < 0x1'00'00'00'00'00 ) {
      write_tag(type_tag::string5);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
      put_byte(byte(str_size, 3));
      put_byte(byte(str_size, 4));
    } else if ( str_size < 0x1'00'00'00'00'00'00 ) {
      write_tag(type_tag::string6);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
      put_byte(byte(str_size, 3));
      put_byte(byte(str_size, 4));
      put_byte(byte(str_size, 5));
    } else if ( str_size < 0x1'00'00'00'00'00'00'00 ) {
      write_tag(type_tag::string7);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
      put_byte(byte(str_size, 3));
      put_byte(byte(str_size, 4));
      put_byte(byte(str_size, 5));
      put_byte(byte(str_size, 6));
    } else {
      write_tag(type_tag::string8);
      put_byte(byte(str_size, 0));
      put_byte(byte(str_size, 1));
      put_byte(byte(str_size, 2));
      put_byte(byte(str_size, 3));
      put_byte(byte(str_size, 4));
      put_byte(byte(str_size, 5));
      put_byte(byte(str_size, 6));
      put_byte(byte(str_size, 7));
    }

    std::copy_n(str.data( ), str_size, as_ptr<char>( ));
    offset_ += str_size;
  }

  void put_byte (uint8_t byte) {
    check_capacity(sizeof(uint8_t));
    as_ref<uint8_t>( ) = byte;
    offset_ += sizeof(uint8_t);
  }

  [[nodiscard]] uint8_t byte (uint64_t v, int byte_index) {
    return static_cast<uint8_t>((v >> (byte_index * 8)) & 0xFF);
  }

  template<std::integral T>
  void write (T val) {
    const uint64_t v = val >= 0 ? static_cast<uint64_t>(val) : -static_cast<uint64_t>(val);
    check_capacity(sizeof(type_tag) + bytes_needed(v));
    if ( val >= 0 ) {
      if ( v < 0x1'00 ) {
        write_tag(type_tag::number1);
        put_byte(byte(v, 0));
      } else if ( v < 0x1'00'00 ) {
        write_tag(type_tag::number2);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
      } else if ( v < 0x1'00'00'00 ) {
        write_tag(type_tag::number3);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
      } else if ( v < 0x1'00'00'00'00 ) {
        write_tag(type_tag::number4);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
      } else if ( v < 0x1'00'00'00'00'00 ) {
        write_tag(type_tag::number5);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
      } else if ( v < 0x1'00'00'00'00'00'00 ) {
        write_tag(type_tag::number6);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
      } else if ( v < 0x1'00'00'00'00'00'00'00 ) {
        write_tag(type_tag::number7);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
        put_byte(byte(v, 6));
      } else {
        write_tag(type_tag::number8);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
        put_byte(byte(v, 6));
        put_byte(byte(v, 7));
      }
    } else {
      if ( v < 0x1'00 ) {
        write_tag(type_tag::neg_number1);
        put_byte(byte(v, 0));
      } else if ( v < 0x1'00'00 ) {
        write_tag(type_tag::neg_number2);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
      } else if ( v < 0x1'00'00'00 ) {
        write_tag(type_tag::neg_number3);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
      } else if ( v < 0x1'00'00'00'00 ) {
        write_tag(type_tag::neg_number4);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
      } else if ( v < 0x1'00'00'00'00'00 ) {
        write_tag(type_tag::neg_number5);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
      } else if ( v < 0x1'00'00'00'00'00'00 ) {
        write_tag(type_tag::neg_number6);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
      } else if ( v < 0x1'00'00'00'00'00'00'00 ) {
        write_tag(type_tag::neg_number7);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
        put_byte(byte(v, 6));
      } else {
        write_tag(type_tag::neg_number8);
        put_byte(byte(v, 0));
        put_byte(byte(v, 1));
        put_byte(byte(v, 2));
        put_byte(byte(v, 3));
        put_byte(byte(v, 4));
        put_byte(byte(v, 5));
        put_byte(byte(v, 6));
        put_byte(byte(v, 7));
      }
    }
  }

  void write (bool val) {
    check_capacity(sizeof(type_tag) + sizeof(bool));
    write_tag(type_tag::boolean);
    as_ref<bool>( ) = val;
    offset_ += sizeof(bool);
  }

  void write (const parameter_list_t& vec) {
    write_tag(type_tag::vector);
    write_len(vec.size( ));
    for ( const auto& item: vec ) {
      serialize(item);
    }
  }

  void write (const parameter_map_t& m) {
    write_tag(type_tag::map);
    write_len(m.size( ));
    for ( const auto& [key, item]: m ) {
      write(key);
      serialize(item);
    }
  }

  void serialize (const parameter_t& param) {
    if ( std::holds_alternative<std::string>(param) ) {
      write(std::get<std::string>(param));
    } else if ( std::holds_alternative<int64_t>(param) ) {
      write(std::get<int64_t>(param));
    } else if ( std::holds_alternative<uint64_t>(param) ) {
      write(static_cast<int64_t>(std::get<uint64_t>(param)));
    } else if ( std::holds_alternative<bool>(param) ) {
      write(std::get<bool>(param));
    } else if ( std::holds_alternative<parameter_list_t>(param) ) {
      write(std::get<parameter_list_t>(param));
    } else if ( std::holds_alternative<parameter_map_t>(param) ) {
      write(std::get<parameter_map_t>(param));
    } else {
      throw error("parameter_bytestream_t: parameter_t holds an unserializable alternative");
    }
  }

  size_t read_len ( ) {
    const type_tag tag = read_tag( );
    switch ( tag ) {
      using enum type_tag;
      case number1:     return read_integer(1);
      case number2:     return read_integer(2);
      case number3:     return read_integer(3);
      case number4:     return read_integer(4);
      case number5:     return read_integer(5);
      case number6:     return read_integer(6);
      case number7:     return read_integer(7);
      case number8:     return read_integer(8);
      default:          throw error(fmt::format("parameter_bytestream_t: invalid type tag {} for length", static_cast<int>(tag)));
    }
  }

  type_tag read_tag ( ) {
    check_capacity(sizeof(type_tag));
    const auto ret = as_ref<type_tag>( );
    offset_ += sizeof(type_tag);
    return ret;
  }

  std::string read_string (int bytes) {
    const size_t len = read_integer(bytes);
    check_capacity(len);
    const std::string_view sv {as_ptr<char>( ), len};
    offset_ += len;
    return std::string {sv};
  }

  uint8_t read_byte ( ) {
    check_capacity(sizeof(uint8_t));
    const uint8_t ret = as_ref<uint8_t>( );
    offset_ += sizeof(uint8_t);
    return ret;
  }

  template<std::integral T = int64_t>
  T read_integer (size_t bytes) {
    int64_t ret = 0;
    for ( size_t i = 0; i < bytes; ++i ) {
      ret |= static_cast<int64_t>(read_byte( )) << (i * 8);
    }
    return static_cast<T>(ret);
  }

  bool read_bool ( ) {
    check_capacity(sizeof(bool));
    const bool ret = as_ref<bool>( );
    offset_ += sizeof(bool);
    return ret;
  }

  parameter_list_t read_vector ( ) {
    const size_t     len = read_len( );
    parameter_list_t vec;
    for ( size_t i = 0; i < len; ++i ) {
      vec.push_back(deserialize( ));
    }
    return vec;
  }

  parameter_map_t read_map ( ) {
    const size_t    len = read_len( );
    parameter_map_t m;
    for ( size_t i = 0; i < len; ++i ) {
      const type_tag    tag = read_tag( );
      const std::string key = [this] (type_tag e) {
        switch ( e ) {
          using enum type_tag;
          case string1: return read_string(1);
          case string2: return read_string(2);
          case string3: return read_string(3);
          case string4: return read_string(4);
          case string5: return read_string(5);
          case string6: return read_string(6);
          case string7: return read_string(7);
          case string8: return read_string(8);
          default:      throw error(fmt::format("parameter_bytestream_t: invalid type tag {} for map key", static_cast<int>(e)));
        }
      }(tag);
      m.emplace(key, deserialize( ));
    }
    return m;
  }

  parameter_t deserialize ( ) {
    const type_tag tag = read_tag( );
    switch ( tag ) {
      using enum type_tag;
      case null:        return { };
      case string1:     return read_string(1);
      case string2:     return read_string(2);
      case string3:     return read_string(3);
      case string4:     return read_string(4);
      case string5:     return read_string(5);
      case string6:     return read_string(6);
      case string7:     return read_string(7);
      case string8:     return read_string(8);
      case number1:     return read_integer(1);
      case number2:     return read_integer(2);
      case number3:     return read_integer(3);
      case number4:     return read_integer(4);
      case number5:     return read_integer(5);
      case number6:     return read_integer(6);
      case number7:     return read_integer(7);
      case number8:     return read_integer(8);
      case neg_number1: return -read_integer(1);
      case neg_number2: return -read_integer(2);
      case neg_number3: return -read_integer(3);
      case neg_number4: return -read_integer(4);
      case neg_number5: return -read_integer(5);
      case neg_number6: return -read_integer(6);
      case neg_number7: return -read_integer(7);
      case neg_number8: return static_cast<int64_t>(-read_integer<uint64_t>(8));
      case boolean:     return read_bool( );
      case vector:      return read_vector( );
      case map:         return read_map( );
    }
    throw error(fmt::format("parameter_bytestream_t: invalid type tag {}", static_cast<int>(tag)));
  }
};

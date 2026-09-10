#include <gtest/gtest.h>

#include <array>
#include <iostream>
#include <limits>

#include <parameter/parameter.hxx>

[[maybe_unused]] parameter_t             p;    // make sure parameter_t is defined and accessible
[[maybe_unused]] parameter_bytestream_t* pbs;  // make sure parameter_bytestream_t is defined and accessible

std::ostream& operator<< (std::ostream& os, const parameter_t& p) {
  std::visit([&os] (const auto& v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr ( std::is_same_v<T, parameter_list_t> ) {
      os << "[";
      for ( const auto& item: v ) {
        os << item << ", ";
      }
      os << "]";
    } else if constexpr ( std::is_same_v<T, parameter_map_t> ) {
      os << "{";
      for ( const auto& [key, item]: v ) {
        os << key << ": " << item << ", ";
      }
      os << "}";
    } else {
      os << v;
    }
  }, p);
  return os;
}

// store() into buffer with one instance, then load() from it with a fresh
// one - mirrors how host and plugin would each use their own instance over
// the same bytes.
result_t<parameter_t> roundtrip (const parameter_t& p, std::span<std::byte> buffer) {
  parameter_bytestream_t pbs_write(buffer);
  if ( const auto written = pbs_write.store(p); !written ) {
    return std::unexpected(written.error( ));
  }

  parameter_bytestream_t pbs_read(buffer);
  return pbs_read.load( );
}

TEST (ParametersBitstream, Integers) {
  for ( const auto i: std::initializer_list<int64_t> {
            -0x08'00'00'12'0f'00'00'00,
            -100,
            -1,
            0,
            1,
            42,
            100,
            1000,
            10000,
            0x7fffffff,
            0x80000000,
            0xffffffff,
            0x7fbcd98ef012378LL,
            0xfffffffeaffffffLL,
            std::numeric_limits<int64_t>::max( ),
            std::numeric_limits<int64_t>::min( ),
        } ) {
    const parameter_t          p_int = i;
    std::array<std::byte, 100> buffer;
    buffer.fill(std::byte {0xfa});

    const auto out = roundtrip(p_int, buffer);
    ASSERT_TRUE(out.has_value( )) << out.error( );
    EXPECT_EQ(*out, p_int);
  }
}

TEST (ParametersBitstream, Strings) {
  const parameter_t          p_str = std::string("Hello, World!");
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_str, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_str);
}

TEST (ParametersBitstream, Booleans) {
  const parameter_t          p_bool = true;
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_bool, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_bool);
}

TEST (ParametersBitstream, IntegersList) {
  const parameter_t          p_list = parameter_list_t {1, 2, 3};
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_list, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_list);
}

TEST (ParametersBitstream, StringsList) {
  const parameter_t          p_list = parameter_list_t {"one", "two", "three"};
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_list, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_list);
}

TEST (ParametersBitstream, IntegersListsList) {
  const parameter_t p_list = parameter_list_t {
      parameter_list_t {1,   2  },
       parameter_list_t {30,  40 },
       parameter_list_t {500, 600}
  };
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_list, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_list);
}

TEST (ParametersBitstream, Map) {
  const parameter_t p_map = parameter_map_t {
      {"0x0",   0                               },
      {"0x1",   1                               },
      {"0x2",   2                               },
      {"0x3",   3                               },
      {"0x4",   4                               },
      {"0x5",   5                               },
      {"0x6",   6                               },
      {"hex",   "hex values"                    },
      {"dec",   "decimal values"                },
      {"bin",   "binary values"                 },
      {"ok",    true                            },
      {"array", parameter_list_t {1, 2, 3, 4, 5}},
      {"map",
       parameter_map_t {
              {"nested_key1", "nested_value1"},
              {"nested_key2", "nested_value2"},
          }                                     },
  };
  std::array<std::byte, 0x1000> buffer;
  buffer.fill(std::byte {0xfa});

  const auto out = roundtrip(p_map, buffer);
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_map);
}

TEST (ParametersBitstream, RejectsReusedInstance) {
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  parameter_bytestream_t pbs(buffer);
  ASSERT_TRUE(pbs.store(parameter_t {int64_t {42}}).has_value( ));

  // offset_ is no longer 0 after the first store() - a second store() or a
  // load() on the same instance would either corrupt the buffer or parse
  // from the middle of it, so both must be refused rather than silently
  // doing the wrong thing.
  EXPECT_FALSE(pbs.store(parameter_t {int64_t {7}}).has_value( ));
  EXPECT_FALSE(pbs.load( ).has_value( ));
}

TEST (ParametersBitstream, RejectsWireVersionMismatch) {
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  parameter_bytestream_t pbs_write(buffer);
  ASSERT_TRUE(pbs_write.store(parameter_t {std::string("x")}).has_value( ));

  buffer[0] = std::byte {parameter_bytestream_t::wire_version + 1};

  parameter_bytestream_t pbs_read(buffer);
  EXPECT_FALSE(pbs_read.load( ).has_value( ));
}

TEST (ParametersBitstream, RejectsCorruptedPayload) {
  std::array<std::byte, 100> buffer;
  buffer.fill(std::byte {0xfa});

  parameter_bytestream_t pbs_write(buffer);
  ASSERT_TRUE(pbs_write.store(parameter_t {std::string("Hello, World!")}).has_value( ));

  // Flip a bit well inside the payload (past the version + length +
  // checksum header) - the bytes still parse as *some* well-formed
  // string, so only the checksum catches this.
  buffer[pbs_write.size( ) - 1] ^= std::byte {0x01};

  parameter_bytestream_t pbs_read(buffer);
  EXPECT_FALSE(pbs_read.load( ).has_value( ));
}

TEST (ParametersBitstream, IgnoresGarbageBeyondTheStoredPayload) {
  // A buffer much bigger than the payload, pre-filled with non-zero
  // "garbage" - e.g. leftover bytes from an earlier store() into the same
  // scratch buffer. load() must trust only the length this store() wrote,
  // not the buffer's full physical size, or it would end up reading that
  // garbage as if it were real trailing data.
  std::array<std::byte, 4096> buffer;
  buffer.fill(std::byte {0xfa});

  parameter_bytestream_t pbs_write(buffer);
  const parameter_t      p_int = int64_t {42};
  ASSERT_TRUE(pbs_write.store(p_int).has_value( ));

  // load() against the *entire* oversized buffer, not just the bytes
  // actually written.
  parameter_bytestream_t pbs_read(buffer);
  const auto              out = pbs_read.load( );
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_int);
}

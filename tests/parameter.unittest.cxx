#include <gtest/gtest.h>

#include <array>
#include <iostream>
#include <limits>

#include <parameter/parameter.hxx>

[[maybe_unused]] parameter_t             p;
[[maybe_unused]] parameter_bytestream_t* pbs;

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

  buffer[pbs_write.size( ) - 1] ^= std::byte {0x01};

  parameter_bytestream_t pbs_read(buffer);
  EXPECT_FALSE(pbs_read.load( ).has_value( ));
}

TEST (ParametersBitstream, IgnoresGarbageBeyondTheStoredPayload) {
  std::array<std::byte, 4096> buffer;
  buffer.fill(std::byte {0xfa});

  parameter_bytestream_t pbs_write(buffer);
  const parameter_t      p_int = int64_t {42};
  ASSERT_TRUE(pbs_write.store(p_int).has_value( ));

  parameter_bytestream_t pbs_read(buffer);
  const auto              out = pbs_read.load( );
  ASSERT_TRUE(out.has_value( )) << out.error( );
  EXPECT_EQ(*out, p_int);
}

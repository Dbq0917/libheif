/*
  libheif unit tests - NAL unit splitter

  MIT License

  Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "catch_amalgamated.hpp"
#include "codecs/decoder.h"

#include <cstdint>
#include <vector>

TEST_CASE("split_nal_units: two well-formed NAL units") {
  std::vector<uint8_t> buf{
      0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC,   // NAL of 3 bytes
      0x00, 0x00, 0x00, 0x02, 0xDD, 0xEE};        // NAL of 2 bytes
  auto units = split_nal_units_4byte_length_prefixed(buf.data(), buf.size());
  REQUIRE(units.size() == 2);
  REQUIRE(units[0].second == 3);
  REQUIRE(units[0].first[0] == 0xAA);
  REQUIRE(units[1].second == 2);
  REQUIRE(units[1].first[0] == 0xDD);
}

TEST_CASE("split_nal_units: length running past the end stops cleanly") {
  // First NAL is fine; second declares 100 bytes but only 2 remain.
  std::vector<uint8_t> buf{
      0x00, 0x00, 0x00, 0x01, 0xAA,
      0x00, 0x00, 0x00, 0x64, 0xBB, 0xCC};
  auto units = split_nal_units_4byte_length_prefixed(buf.data(), buf.size());
  REQUIRE(units.size() == 1);
  REQUIRE(units[0].second == 1);
  REQUIRE(units[0].first[0] == 0xAA);
}

TEST_CASE("split_nal_units: zero-length NAL is skipped, parsing continues") {
  std::vector<uint8_t> buf{
      0x00, 0x00, 0x00, 0x00,                     // zero-length NAL
      0x00, 0x00, 0x00, 0x01, 0x7E};              // 1-byte NAL
  auto units = split_nal_units_4byte_length_prefixed(buf.data(), buf.size());
  REQUIRE(units.size() == 1);
  REQUIRE(units[0].second == 1);
  REQUIRE(units[0].first[0] == 0x7E);
}

TEST_CASE("split_nal_units: a lone truncated length prefix yields nothing") {
  std::vector<uint8_t> buf{0x00, 0x00, 0x00};  // fewer than 4 bytes
  auto units = split_nal_units_4byte_length_prefixed(buf.data(), buf.size());
  REQUIRE(units.empty());
}

TEST_CASE("split_nal_units: empty input yields nothing") {
  auto units = split_nal_units_4byte_length_prefixed(nullptr, 0);
  REQUIRE(units.empty());
}

namespace {

// Build a length-prefixed NAL using the given prefix width (1, 2 or 4 bytes).
std::vector<uint8_t> prefixed_nal(uint8_t length_size, const std::vector<uint8_t>& payload)
{
  std::vector<uint8_t> out;
  const uint32_t len = static_cast<uint32_t>(payload.size());
  for (int shift = (length_size - 1) * 8; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((len >> shift) & 0xFF));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

void append_vector(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src)
{
  dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

TEST_CASE("reframe: 2-byte sample prefixes are expanded to 4-byte prefixes") {
  // One configuration NAL with the canonical 4-byte prefix, followed by sample
  // NAL units that use 2-byte length fields (as phone footage does).
  std::vector<uint8_t> config = prefixed_nal(4, {0x40, 0x01});
  std::vector<uint8_t> sample;
  append_vector(sample, prefixed_nal(2, {0x26, 0x01, 0xAA}));
  append_vector(sample, prefixed_nal(2, {0x28, 0x02}));

  std::vector<uint8_t> buffer = config;
  buffer.insert(buffer.end(), sample.begin(), sample.end());

  auto result = reframe_nal_units_to_4byte_length(buffer, config.size(), 2,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(!result.is_error());

  // The output must be exactly the same NAL units with 4-byte prefixes.
  std::vector<uint8_t> expected = config;
  append_vector(expected, prefixed_nal(4, {0x26, 0x01, 0xAA}));
  append_vector(expected, prefixed_nal(4, {0x28, 0x02}));
  REQUIRE(*result == expected);

  // And it must be splittable with the 4-byte walker the plugins use.
  auto units = split_nal_units_4byte_length_prefixed(result->data(), result->size());
  REQUIRE(units.size() == 3);
  REQUIRE(units[1].second == 3);
  REQUIRE(units[2].second == 2);
}

TEST_CASE("reframe: 1-byte prefixes are supported too") {
  std::vector<uint8_t> buffer = prefixed_nal(1, {0x65, 0xAB});
  auto result = reframe_nal_units_to_4byte_length(buffer, 0, 1,
                                                  heif_compression_AVC, "avcC");
  REQUIRE(!result.is_error());
  REQUIRE(*result == prefixed_nal(4, {0x65, 0xAB}));
}

TEST_CASE("reframe: 4-byte width leaves every existing buffer unchanged") {
  std::vector<uint8_t> config = prefixed_nal(4, {0x40, 0x01});
  std::vector<uint8_t> buffer = config;
  append_vector(buffer, prefixed_nal(4, {0x26, 0x01}));

  auto result = reframe_nal_units_to_4byte_length(buffer, config.size(), 4,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(!result.is_error());
  // The 4-byte path performs no reframing: every byte survives unchanged, so
  // files that already used 4-byte length fields decode exactly as before.
  REQUIRE(*result == buffer);
}

TEST_CASE("reframe: the reserved 3-byte width is explicitly rejected") {
  const uint32_t before = get_nal_length_field_anomaly_count();
  std::vector<uint8_t> buffer = prefixed_nal(3, {0x26, 0x01, 0x02});
  auto result = reframe_nal_units_to_4byte_length(buffer, 0, 3,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(result.is_error());
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
  REQUIRE(result.error().sub_error_code == heif_suberror_Invalid_parameter_value);
  REQUIRE(get_nal_length_field_anomaly_count() == before + 1);
}

TEST_CASE("reframe: a truncated length prefix is rejected and counted") {
  const uint32_t before = get_nal_length_field_anomaly_count();
  std::vector<uint8_t> config = prefixed_nal(4, {0x40, 0x01});
  std::vector<uint8_t> buffer = config;
  buffer.push_back(0x00); // a 2-byte prefix with only one byte left

  auto result = reframe_nal_units_to_4byte_length(buffer, config.size(), 2,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(result.is_error());
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
  REQUIRE(get_nal_length_field_anomaly_count() == before + 1);
}

TEST_CASE("reframe: a length running past the sample end is rejected and counted") {
  const uint32_t before = get_nal_length_field_anomaly_count();
  std::vector<uint8_t> buffer;
  append_vector(buffer, prefixed_nal(2, {0x26, 0x01}));
  buffer.push_back(0x00);
  buffer.push_back(0x10); // claims 16 payload bytes, none follow

  auto result = reframe_nal_units_to_4byte_length(buffer, 0, 2,
                                                  heif_compression_AVC, "avcC");
  REQUIRE(result.is_error());
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
  REQUIRE(get_nal_length_field_anomaly_count() == before + 1);
}

TEST_CASE("reframe: empty sample data after configuration is valid") {
  std::vector<uint8_t> config = prefixed_nal(4, {0x40, 0x01});
  auto result = reframe_nal_units_to_4byte_length(config, config.size(), 2,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(!result.is_error());
  REQUIRE(*result == config);
}

TEST_CASE("reframe: zero-length 2-byte NAL is preserved like the 4-byte path") {
  std::vector<uint8_t> buffer;
  append_vector(buffer, prefixed_nal(2, {}));       // zero-length NAL
  append_vector(buffer, prefixed_nal(2, {0x26, 1}));

  auto result = reframe_nal_units_to_4byte_length(buffer, 0, 2,
                                                  heif_compression_HEVC, "hvcC");
  REQUIRE(!result.is_error());

  std::vector<uint8_t> expected;
  append_vector(expected, prefixed_nal(4, {}));
  append_vector(expected, prefixed_nal(4, {0x26, 1}));
  REQUIRE(*result == expected);
}

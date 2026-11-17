/*
  libheif unit tests - NAL length field width from codec configuration records

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
#include "codecs/avc_boxes.h"
#include "codecs/avc_dec.h"
#include "codecs/decoder.h"
#include "codecs/hevc_boxes.h"
#include "codecs/hevc_dec.h"
#include "codecs/vvc_boxes.h"
#include "codecs/vvc_dec.h"
#include "error.h"
#include "libheif/heif.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

std::vector<uint8_t> sample_with_2byte_prefixes()
{
  // Two sample NAL units with 2-byte big-endian length prefixes.
  return {
      0x00, 0x03, 0x26, 0x01, 0xAA,
      0x00, 0x02, 0x28, 0x02};
}

} // namespace

TEST_CASE("HEVC decoder reports the hvcC length field width per sample description")
{
  auto hvcc = std::make_shared<Box_hvcC>();

  hvcc->get_configuration().m_length_size = 4;
  Decoder_HEVC decoder4(hvcc);
  CHECK(decoder4.get_nal_length_field_size() == 4);
  CHECK(std::string(decoder4.get_configuration_box_type()) == "hvcC");

  // A replaced configuration record changes the returned width immediately:
  // the value is read on every call instead of being globally cached.
  hvcc->get_configuration().m_length_size = 2;
  CHECK(decoder4.get_nal_length_field_size() == 2);

  // A second track/sample description with its own hvcC keeps an independent
  // width even while the first one still exists.
  auto other = std::make_shared<Box_hvcC>();
  other->get_configuration().m_length_size = 1;
  Decoder_HEVC decoder1(other);
  CHECK(decoder1.get_nal_length_field_size() == 1);
  CHECK(decoder4.get_nal_length_field_size() == 2);
}

TEST_CASE("AVC decoder reports the avcC length field width")
{
  auto avcc = std::make_shared<Box_avcC>();
  avcc->get_configuration().lengthSize = 2;
  Decoder_AVC decoder(avcc);
  CHECK(decoder.get_nal_length_field_size() == 2);
  CHECK(std::string(decoder.get_configuration_box_type()) == "avcC");
}

TEST_CASE("VVC decoder derives the width from vvcC LengthSizeMinusOne")
{
  auto vvcc = std::make_shared<Box_vvcC>();
  vvcc->get_configuration().LengthSizeMinusOne = 1; // width 2
  Decoder_VVC decoder(vvcc);
  CHECK(decoder.get_nal_length_field_size() == 2);
  CHECK(std::string(decoder.get_configuration_box_type()) == "vvcC");

  vvcc->get_configuration().LengthSizeMinusOne = 3; // width 4
  CHECK(decoder.get_nal_length_field_size() == 4);
}

TEST_CASE("2-byte HEVC samples are reframed for the 4-byte plugin contract")
{
  auto hvcc = std::make_shared<Box_hvcC>();
  hvcc->get_configuration().m_length_size = 2;
  Decoder_HEVC decoder(hvcc);

  auto sample = sample_with_2byte_prefixes();
  auto result = reframe_nal_units_to_4byte_length(
      sample, 0, decoder.get_nal_length_field_size(),
      decoder.get_compression_format(), decoder.get_configuration_box_type());

  REQUIRE(!result.is_error());
  REQUIRE(*result == std::vector<uint8_t>{
      0x00, 0x00, 0x00, 0x03, 0x26, 0x01, 0xAA,
      0x00, 0x00, 0x00, 0x02, 0x28, 0x02});
}

TEST_CASE("Reserved 3-byte width is rejected through every NAL codec")
{
  const uint32_t before = get_nal_length_field_anomaly_count();
  std::vector<uint8_t> sample{0x00, 0x01, 0x67};

  for (auto [codec, box_type] : {
      std::pair{heif_compression_AVC, "avcC"},
      std::pair{heif_compression_HEVC, "hvcC"},
      std::pair{heif_compression_VVC, "vvcC"}}) {
    auto result = reframe_nal_units_to_4byte_length(sample, 0, 3, codec, box_type);
    REQUIRE(result.is_error());
    CHECK(result.error().sub_error_code == heif_suberror_Invalid_parameter_value);
  }

  // One anomaly per rejected sample, not per codec table entry accident.
  CHECK(get_nal_length_field_anomaly_count() == before + 3);
}

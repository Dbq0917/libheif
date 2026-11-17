/*
  libheif regression tests: multi-segment image sequences.

  MIT License

  Copyright (c) 2026 libheif contributors

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
#include "libheif/heif.h"
#include "libheif/heif_plugin.h"
#include "libheif/heif_sequences.h"
#include "sequence_file_builder.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace seqfile;

namespace {

int corrupt_sample = -1;
int new_decoder_count = 0;
int output_latency = 0;

struct MockDecoderState
{
  std::deque<uintptr_t> pending_samples;
  bool flushed = false;
};

const heif_error mock_ok = {heif_error_Ok, heif_suberror_Unspecified, "Success"};

const char* mock_plugin_name() { return "mock multi-segment decoder"; }

int mock_does_support_format(heif_compression_format format)
{
  return format == heif_compression_HEVC ? 1000 : 0;
}

int mock_does_support_format2(const heif_decoder_plugin_compressed_format_description* desc)
{
  return mock_does_support_format(desc->format);
}

heif_error mock_new_decoder(void** decoder)
{
  *decoder = new MockDecoderState();
  new_decoder_count++;
  return mock_ok;
}

heif_error mock_new_decoder2(void** decoder, const heif_decoder_plugin_options*)
{
  return mock_new_decoder(decoder);
}

void mock_free_decoder(void* decoder)
{
  delete static_cast<MockDecoderState*>(decoder);
}

heif_error mock_push_data2(void* decoder, const void*, size_t, uintptr_t user_data)
{
  if (static_cast<int>(user_data) == corrupt_sample) {
    return {heif_error_Invalid_input, heif_suberror_Unspecified, "corrupt test segment"};
  }

  static_cast<MockDecoderState*>(decoder)->pending_samples.push_back(user_data);
  return mock_ok;
}

heif_error mock_push_data(void* decoder, const void* data, size_t size)
{
  return mock_push_data2(decoder, data, size, 0);
}

heif_error mock_flush_data(void* decoder)
{
  static_cast<MockDecoderState*>(decoder)->flushed = true;
  return mock_ok;
}

heif_error mock_decode_next_image2(void* decoder, heif_image** out_img, uintptr_t* out_user_data,
                                   const heif_security_limits*)
{
  *out_img = nullptr;

  auto* state = static_cast<MockDecoderState*>(decoder);
  if (state->pending_samples.empty() ||
      (!state->flushed && state->pending_samples.size() <= static_cast<size_t>(output_latency))) {
    return mock_ok;
  }

  const uintptr_t sample = state->pending_samples.front();
  state->pending_samples.pop_front();

  heif_error err = heif_image_create(4, 4, heif_colorspace_monochrome, heif_chroma_monochrome, out_img);
  if (err.code) {
    return err;
  }

  err = heif_image_add_plane(*out_img, heif_channel_Y, 4, 4, 8);
  if (err.code) {
    heif_image_release(*out_img);
    *out_img = nullptr;
    return err;
  }

  int stride = 0;
  uint8_t* pixels = heif_image_get_plane(*out_img, heif_channel_Y, &stride);
  if (!pixels) {
    heif_image_release(*out_img);
    *out_img = nullptr;
    return {heif_error_Memory_allocation_error, heif_suberror_Unspecified, "no plane"};
  }

  // A distinct value per sample makes it possible to check that later frames
  // (and not only the first segment) are returned.
  std::memset(pixels, static_cast<int>(sample) + 1, static_cast<size_t>(4) * static_cast<size_t>(stride));

  if (out_user_data) {
    *out_user_data = sample;
  }
  return mock_ok;
}

heif_error mock_decode_next_image(void* decoder, heif_image** out_img, const heif_security_limits* limits)
{
  return mock_decode_next_image2(decoder, out_img, nullptr, limits);
}

heif_error mock_decode_image(void* decoder, heif_image** out_img)
{
  return mock_decode_next_image2(decoder, out_img, nullptr, nullptr);
}

void mock_set_strict_decoding(void*, int) {}

const heif_decoder_plugin mock_decoder_plugin{
  5,
  mock_plugin_name,
  nullptr,
  nullptr,
  mock_does_support_format,
  mock_new_decoder,
  mock_free_decoder,
  mock_push_data,
  mock_decode_image,
  mock_set_strict_decoding,
  "mock-multisegment",
  mock_decode_next_image,
  LIBHEIF_MAKE_VERSION(1, 21, 0),
  mock_does_support_format2,
  mock_new_decoder2,
  mock_push_data2,
  mock_flush_data,
  mock_decode_next_image2
};

void ensure_mock_decoder_registered()
{
  static bool registered = false;
  if (!registered) {
    REQUIRE(heif_register_decoder_plugin(&mock_decoder_plugin).code == heif_error_Ok);
    registered = true;
  }
}

std::vector<uint8_t> make_visual_entry(const char* type)
{
  std::vector<uint8_t> header;
  for (int i = 0; i < 6; i++) header.push_back(0); // SampleEntry reserved
  put16(header, 1);                                // data_reference_index
  put16(header, 0);
  put16(header, 0);
  for (int i = 0; i < 3; i++) put32(header, 0);
  put16(header, 4);
  put16(header, 4);
  put32(header, 0x00480000);
  put32(header, 0x00480000);
  put32(header, 0);
  put16(header, 1);
  for (int i = 0; i < 32; i++) header.push_back(0);
  put16(header, 0x0018);
  put16(header, 0xFFFF);

  // Minimal hvcC with no parameter-set arrays. The mock decoder does not use it.
  std::vector<uint8_t> hvcc;
  hvcc.push_back(1);
  hvcc.push_back(0);
  put32(hvcc, 0);
  for (int i = 0; i < 6; i++) hvcc.push_back(0);
  hvcc.push_back(0);
  put16(hvcc, 0);
  hvcc.push_back(0);
  hvcc.push_back(1);
  hvcc.push_back(0);
  hvcc.push_back(0);
  put16(hvcc, 0);
  hvcc.push_back(3);
  hvcc.push_back(0);
  append(header, box("hvcC", hvcc));

  return box(type, header);
}

std::vector<uint8_t> build_six_segment_sequence(const std::vector<uint32_t>& durations,
                                                bool alternate_descriptions = true)
{
  SequenceFileParams p;
  p.mvhd_duration = 0;
  p.timescale = 1000;
  p.with_editlist = false;
  p.mdhd_duration = 0;
  p.num_samples = static_cast<uint32_t>(durations.size());
  p.stts.clear();
  p.handler_type = "pict";
  // A nonzero value creates the second sample entry used for parameter changes.
  p.first_chunk_num_samples = alternate_descriptions ? 1 : 0;
  p.sample_entry = make_visual_entry("hvc1");
  p.second_sample_entry = make_visual_entry("hvc1");
  // A length-prefixed AUD NAL is enough for the sequence decoder's framing
  // checks; the mock decoder ignores the payload.
  p.sample_bytes = {0, 0, 0, 2, 0x46, 0x01};
  p.second_sample_bytes = {0, 0, 0, 2, 0x46, 0x01};

  uint64_t total_duration = 0;
  for (size_t i = 0; i < durations.size(); i++) {
    p.stts.push_back({1, durations[i]});
    total_duration += durations[i];

    Stsc_entry entry{};
    entry.first_chunk = static_cast<uint32_t>(i + 1);
    entry.samples_per_chunk = 1;
    // Alternate descriptions so every segment has to rebuild its decoder
    // environment instead of inheriting the previous segment's settings.
    entry.sample_description_index = (alternate_descriptions && (i % 2 != 0)) ? 2 : 1;
    p.stsc_entries.push_back(entry);
  }
  p.num_chunks = static_cast<uint32_t>(durations.size());
  p.mdhd_duration = total_duration;
  p.mvhd_duration = total_duration;

  std::vector<uint8_t> file = build_sequence_file(p);

  return file;
}

struct DecodeResult
{
  std::vector<uint32_t> durations;
  std::vector<uint8_t> first_pixels;
  heif_error error = {heif_error_Ok, heif_suberror_Unspecified, nullptr};
};

DecodeResult decode_sequence(const std::vector<uint8_t>& file)
{
  DecodeResult result;

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  result.error = heif_context_read_from_memory(ctx, file.data(), file.size(), nullptr);
  REQUIRE(result.error.code == heif_error_Ok);
  REQUIRE(heif_context_has_sequence(ctx) == 1);
  heif_track* track = heif_context_get_track(ctx, 0);
  REQUIRE(track != nullptr);

  for (;;) {
    heif_image* image = nullptr;
    heif_error err = heif_track_decode_next_image(track, &image, heif_colorspace_undefined,
                                                  heif_chroma_undefined, nullptr);
    if (err.code == heif_error_End_of_sequence) {
      break;
    }
    if (err.code != heif_error_Ok) {
      result.error = err;
      break;
    }

    REQUIRE(image != nullptr);
    result.durations.push_back(heif_image_get_duration(image));

    size_t stride = 0;
    const uint8_t* pixels = heif_image_get_plane_readonly2(image, heif_channel_Y, &stride);
    REQUIRE(pixels != nullptr);
    result.first_pixels.push_back(pixels[0]);
    heif_image_release(image);
  }

  heif_track_release(track);
  heif_context_free(ctx);
  return result;
}

} // namespace


TEST_CASE("six-segment sequence decodes every segment and keeps its timing")
{
  ensure_mock_decoder_registered();
  corrupt_sample = -1;
  new_decoder_count = 0;
  output_latency = 0;

  const std::vector<uint32_t> durations = {100, 200, 300, 400, 500, 600};
  const auto file = build_six_segment_sequence(durations);

  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  REQUIRE(heif_context_read_from_memory(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);
  CHECK(heif_context_get_sequence_duration(ctx) == 2100);
  heif_context_free(ctx);

  const DecodeResult decoded = decode_sequence(file);
  REQUIRE(decoded.error.code == heif_error_Ok);
  CHECK(decoded.durations == durations);
  CHECK(decoded.first_pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6});

  // Alternating sample descriptions must start a fresh decoder environment for
  // every segment; no segment may reuse the preceding segment's configuration.
  CHECK(new_decoder_count == 6);
}


TEST_CASE("a damaged middle segment is skipped and later segments still decode")
{
  ensure_mock_decoder_registered();
  corrupt_sample = 2;
  new_decoder_count = 0;
  output_latency = 0;

  const std::vector<uint32_t> durations = {100, 200, 300, 400, 500, 600};
  const auto file = build_six_segment_sequence(durations);

  const DecodeResult decoded = decode_sequence(file);
  REQUIRE(decoded.error.code == heif_error_Ok);
  CHECK(decoded.durations == std::vector<uint32_t>{100, 200, 400, 500, 600});
  CHECK(decoded.first_pixels == std::vector<uint8_t>{1, 2, 4, 5, 6});

  corrupt_sample = -1;
}


TEST_CASE("skipping a damaged segment keeps buffered frames from the previous segment")
{
  ensure_mock_decoder_registered();
  corrupt_sample = 2;
  new_decoder_count = 0;
  output_latency = 1;

  const std::vector<uint32_t> durations = {100, 200, 300, 400, 500, 600};
  // All chunks share one sample description, so consecutive chunks also share
  // one decoder instance and its output can lag behind the pushed samples.
  const auto file = build_six_segment_sequence(durations, false);

  const DecodeResult decoded = decode_sequence(file);
  REQUIRE(decoded.error.code == heif_error_Ok);
  CHECK(decoded.durations == std::vector<uint32_t>{100, 200, 400, 500, 600});
  CHECK(decoded.first_pixels == std::vector<uint8_t>{1, 2, 4, 5, 6});

  corrupt_sample = -1;
  output_latency = 0;
}


TEST_CASE("a damaged last segment is dropped at the end of the sequence")
{
  ensure_mock_decoder_registered();
  corrupt_sample = 5;
  output_latency = 0;

  const std::vector<uint32_t> durations = {100, 200, 300, 400, 500, 600};
  const auto file = build_six_segment_sequence(durations);

  const DecodeResult decoded = decode_sequence(file);
  REQUIRE(decoded.error.code == heif_error_Ok);
  CHECK(decoded.durations == std::vector<uint32_t>{100, 200, 300, 400, 500});

  corrupt_sample = -1;
}


TEST_CASE("single-segment decoding still reports a damaged segment")
{
  ensure_mock_decoder_registered();
  corrupt_sample = 0;
  output_latency = 0;

  const std::vector<uint32_t> durations = {100};
  const auto file = build_six_segment_sequence(durations);

  const DecodeResult decoded = decode_sequence(file);
  CHECK(decoded.error.code == heif_error_Invalid_input);

  corrupt_sample = -1;
}

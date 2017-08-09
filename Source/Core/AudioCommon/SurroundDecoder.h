// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/FixedSizeQueue.h"
#include "FreeSurround/FreeSurroundDecoder.h"

class Mixer;

namespace AudioCommon
{
class SurroundDecoder
{
public:
  explicit SurroundDecoder(unsigned int sample_rate);
  void GetDecodedSamples(float* out, unsigned int num_out, Mixer* mixer);
  void Clear();

private:
  static constexpr u32 MAX_SAMPLES = 1024 * 4;  // 128 ms
  std::array<short, MAX_SAMPLES * 2> m_scratch_buffer;
  std::array<float, MAX_SAMPLES * 2> m_float_conversion_buffer;

  unsigned int m_sample_rate;

  DPL2FSDecoder m_fsdecoder;
  const unsigned int SURROUND_FRAMES_PER_CALL = 512;
  const unsigned int SURROUND_CHANNELS = 6;
  FixedSizeQueue<float, 32768> m_decoded_fifo;
};

}  // AudioCommon

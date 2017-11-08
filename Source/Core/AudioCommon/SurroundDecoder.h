// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"
#include "Common/FixedSizeQueue.h"

class DPL2FSDecoder;

namespace AudioCommon
{
class SurroundDecoder
{
public:
  explicit SurroundDecoder(uint32_t sample_rate, uint32_t frame_block_size);
  uint32_t QueryFramesNeededForSurroundOutput(uint32_t output_frames);
  void PutFrames(short* in, size_t num_frames_in);
  void ReceiveFrames(float* out, size_t num_frames_out);
  void Clear();

private:
  uint32_t m_sample_rate;
  uint32_t m_frame_block_size;

  std::shared_ptr<DPL2FSDecoder> m_fsdecoder;
  const uint32_t STEREO_CHANNELS = 2;
  const uint32_t SURROUND_CHANNELS = 6;
  std::array<float, 32768> m_float_conversion_buffer;
  FixedSizeQueue<float, 32768> m_decoded_fifo;
};

}  // AudioCommon

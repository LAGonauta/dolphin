// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <limits>

#include "AudioCommon/SurroundDecoder.h"

namespace AudioCommon
{
SurroundDecoder::SurroundDecoder(uint32_t sample_rate, uint32_t frame_block_size)
{
  m_sample_rate = sample_rate;
  m_frame_block_size = frame_block_size;
  m_fsdecoder = std::make_unique<DPL2FSDecoder>();
  m_fsdecoder->Init(cs_5point1, m_frame_block_size, m_sample_rate);
}

void SurroundDecoder::Clear()
{
  m_fsdecoder->flush();
  m_decoded_fifo.clear();
}

// Currently only 6 channels are supported.
uint32_t SurroundDecoder::QueryFramesNeededForSurroundOutput(uint32_t output_frames)
{
  if (m_decoded_fifo.size() < output_frames * SURROUND_CHANNELS)
  {
    // Output stereo frames needed to have at least the desired number of surround frames
    uint32_t frames_needed =
        output_frames - static_cast<uint32_t>(m_decoded_fifo.size()) / SURROUND_CHANNELS;
    return frames_needed + m_frame_block_size - frames_needed % m_frame_block_size;
  }
  else
  {
    return 0;
  }
}

// Receive and decode samples
void SurroundDecoder::PutFrames(short* in, size_t num_frames_in)
{
  // Maybe check if it is really power-of-2?
  int remaining_frames = static_cast<int>(num_frames_in);
  size_t frame_index = 0;

  while (remaining_frames > 0)
  {
    // Convert to float
    for (size_t i = 0, end = m_frame_block_size * STEREO_CHANNELS; i < end; ++i)
    {
      m_float_conversion_buffer[i] = in[i + frame_index * STEREO_CHANNELS] /
                                     static_cast<float>(std::numeric_limits<short>::max());
    }

    // Decode
    float* dpl2_fs = m_fsdecoder->decode(m_float_conversion_buffer.data());

    // Add to ring buffer and fix channel mapping
    // Maybe modify FreeSurround to output the correct mapping?
    // FreeSurround:
    // FL | FC | FR | BL | BR | LFE
    // Most backends:
    // FL | FR | FC | LFE | BL | BR
    for (size_t i = 0; i < m_frame_block_size; ++i)
    {
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 0 /*LEFTFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 2 /*RIGHTFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 1 /*CENTREFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 5 /*sub/lfe*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 3 /*LEFTREAR*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 4 /*RIGHTREAR*/]);
    }

    remaining_frames = remaining_frames - static_cast<int>(m_frame_block_size);
    frame_index = frame_index + m_frame_block_size;
  }
}

void SurroundDecoder::ReceiveFrames(float* out, size_t num_frames_out)
{
  // Copy to output array with desired num_frames_out
  for (size_t i = 0, num_samples_output = num_frames_out * SURROUND_CHANNELS;
       i < num_samples_output; ++i)
  {
    out[i] = m_decoded_fifo.pop_front();
  }
}

}  // namespace AudioCommon

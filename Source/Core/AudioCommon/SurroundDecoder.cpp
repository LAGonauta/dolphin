// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SurroundDecoder.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"

namespace AudioCommon
{
SurroundDecoder::SurroundDecoder(unsigned int sample_rate)
{
  m_sample_rate = sample_rate;
  m_fsdecoder.Init(cs_5point1, SURROUND_FRAMES_PER_CALL, m_sample_rate);
}

void SurroundDecoder::Clear()
{
  m_fsdecoder.flush();
  m_decoded_fifo.clear();
}

void SurroundDecoder::GetDecodedSamples(float* out, unsigned int num_out, Mixer *m_mixer)
{
  // Calculate how many times we need to request the FS decoder
  size_t num_fs_dec_requests = 0;
  if (m_decoded_fifo.size() < static_cast<size_t>(num_out * SURROUND_CHANNELS))
  {
    int num_frames_remaining =
      num_out - static_cast<u32>(m_decoded_fifo.size() / SURROUND_CHANNELS);
    while (num_frames_remaining > 0)
    {
      ++num_fs_dec_requests;
      num_frames_remaining = num_frames_remaining - SURROUND_FRAMES_PER_CALL;
    }
  }

  while (num_fs_dec_requests > 0)
  {
    // Mix() may also use m_scratch_buffer internally, but is safe because it alternates reads and
    // writes.
    m_mixer->Mix(m_scratch_buffer.data(), SURROUND_FRAMES_PER_CALL);

    // We need to drop any sample after SURROUND_FRAMES_PER_CALL, unless we add another FIFO here
    for (u32 i = 0; i < SURROUND_FRAMES_PER_CALL * 2; ++i)
    {
      m_float_conversion_buffer[i] =
        m_scratch_buffer[i] / static_cast<float>(std::numeric_limits<short>::max());
    }

    // FSDPL2Decode
    float* dpl2_fs = m_fsdecoder.decode(m_float_conversion_buffer.data());

    // Add to queue and fix channel mapping
    // Maybe modify FreeSurround to output the correct mapping?
    // FreeSurround:
    // FL | FC | FR | BL | BR | LFE
    // Most backends:
    // FL | FR | FC | LFE | BL | BR
    for (u32 i = 0; i < SURROUND_FRAMES_PER_CALL; ++i)
    {
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 0 /*LEFTFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 2 /*RIGHTFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 1 /*CENTREFRONT*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 5 /*sub/lfe*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 3 /*LEFTREAR*/]);
      m_decoded_fifo.push(dpl2_fs[i * SURROUND_CHANNELS + 4 /*RIGHTREAR*/]);
    }

    --num_fs_dec_requests;
  }

  // Copy to output array with desired num_samples
  for (u32 i = 0, num_samples_output = num_out * SURROUND_CHANNELS; i < num_samples_output; ++i)
  {
    out[i] = m_decoded_fifo.pop_front();
  }
}

}  // namespace AudioCommon

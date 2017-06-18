// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <climits>
#include <cstring>
#include <thread>

#include "AudioCommon/OpenALStream.h"
#include "AudioCommon/aldlist.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"

#if defined HAVE_OPENAL && HAVE_OPENAL

#ifdef _WIN32
#pragma comment(lib, "openal32.lib")
#endif

//
// AyuanX: Spec says OpenAL1.1 is thread safe already
//
bool OpenALStream::Start()
{
  m_run_thread.Set();
  bool b_return = false;

  ALDeviceList device_list;
  if (device_list.GetNumDevices())
  {
    char* def_dev_name = device_list.GetDeviceName(device_list.GetDefaultDevice());

    INFO_LOG(AUDIO, "Found OpenAL device %s", def_dev_name);

    ALCdevice* device = alcOpenDevice(def_dev_name);
    if (device)
    {
      ALCcontext* context = alcCreateContext(device, nullptr);
      if (context)
      {
        // Used to determine an appropriate period size (2x period = total buffer size)
        // ALCint refresh;
        // alcGetIntegerv(device, ALC_REFRESH, 1, &refresh);
        // period_size_in_millisec = 1000 / refresh;

        alcMakeContextCurrent(context);
        thread = std::thread(&OpenALStream::SoundLoop, this);
        b_return = true;
      }
      else
      {
        alcCloseDevice(device);
        PanicAlertT("OpenAL: can't create context for device %s", def_dev_name);
      }
    }
    else
    {
      PanicAlertT("OpenAL: can't open device %s", def_dev_name);
    }
  }
  else
  {
    PanicAlertT("OpenAL: can't find sound devices");
  }

  return b_return;
}

void OpenALStream::Stop()
{
  m_run_thread.Clear();
  // kick the thread if it's waiting
  sound_sync_event.Set();

  thread.join();

  alSourceStop(source);
  alSourcei(source, AL_BUFFER, 0);

  // Clean up buffers and sources
  alDeleteSources(1, &source);
  source = 0;
  alDeleteBuffers(OAL_BUFFERS, buffers.data());

  ALCcontext* context = alcGetCurrentContext();
  ALCdevice* device = alcGetContextsDevice(context);

  alcMakeContextCurrent(nullptr);
  alcDestroyContext(context);
  alcCloseDevice(device);
}

void OpenALStream::SetVolume(int volume)
{
  m_volume = (float)volume / 100.0f;

  if (source)
    alSourcef(source, AL_GAIN, m_volume);
}

void OpenALStream::Update()
{
  sound_sync_event.Set();
}

void OpenALStream::Clear(bool mute)
{
  m_muted = mute;

  if (m_muted)
  {
    alSourceStop(source);
  }
  else
  {
    alSourcePlay(source);
  }
}

static ALenum CheckALError(const char* desc)
{
  ALenum err = alGetError();

  if (err != AL_NO_ERROR)
  {
    std::string type;

    switch (err)
    {
    case AL_INVALID_NAME:
      type = "AL_INVALID_NAME";
      break;
    case AL_INVALID_ENUM:
      type = "AL_INVALID_ENUM";
      break;
    case AL_INVALID_VALUE:
      type = "AL_INVALID_VALUE";
      break;
    case AL_INVALID_OPERATION:
      type = "AL_INVALID_OPERATION";
      break;
    case AL_OUT_OF_MEMORY:
      type = "AL_OUT_OF_MEMORY";
      break;
    default:
      type = "UNKNOWN_ERROR";
      break;
    }

    ERROR_LOG(AUDIO, "Error %s: %08x %s", desc, err, type.c_str());
  }

  return err;
}

static bool IsCreativeXFi()
{
  return strstr(alGetString(AL_RENDERER), "X-Fi") != nullptr;
}

void OpenALStream::SoundLoop()
{
  Common::SetCurrentThreadName("Audio thread - openal");

  bool float32_capable = alIsExtensionPresent("AL_EXT_float32") != 0;
  bool surround_capable = alIsExtensionPresent("AL_EXT_MCFORMATS") || IsCreativeXFi();
  bool use_surround = SConfig::GetInstance().bDPL2Decoder && surround_capable;

  // As there is no extension to check for 32-bit fixed point support
  // and we know that only a X-Fi with hardware OpenAL supports it,
  // we just check if one is being used.
  bool fixed32_capable = IsCreativeXFi();

  u32 frequency = m_mixer->GetSampleRate();

  u32 frames_per_buffer;
  // Can't have zero samples per buffer
  if (SConfig::GetInstance().iLatency > 0)
  {
    frames_per_buffer = frequency / 1000 * SConfig::GetInstance().iLatency / OAL_BUFFERS;
  }
  else
  {
    frames_per_buffer = frequency / 1000 * 1 / OAL_BUFFERS;
  }

  if (frames_per_buffer > OAL_MAX_FRAMES)
  {
    frames_per_buffer = OAL_MAX_FRAMES;
  }

  INFO_LOG(AUDIO, "Using %d buffers, each with %d audio frames for a total of %d.", OAL_BUFFERS,
           frames_per_buffer, frames_per_buffer * OAL_BUFFERS);

  // Should we make these larger just in case the mixer ever sends more samples
  // than what we request?
  realtime_buffer.resize(frames_per_buffer * STEREO_CHANNELS);
  source = 0;

  // Clear error state before querying or else we get false positives.
  ALenum err = alGetError();

  // Generate some AL Buffers for streaming
  alGenBuffers(OAL_BUFFERS, (ALuint*)buffers.data());
  err = CheckALError("generating buffers");

  // Generate a Source to playback the Buffers
  alGenSources(1, &source);
  err = CheckALError("generating sources");

  // Set the default sound volume as saved in the config file.
  alSourcef(source, AL_GAIN, m_volume);

  // TODO: Error handling
  // ALenum err = alGetError();

  unsigned int next_buffer = 0;
  unsigned int num_buffers_queued = 0;
  ALint state = 0;

  while (m_run_thread.IsSet())
  {
    // Block until we have a free buffer
    int num_buffers_processed;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &num_buffers_processed);
    if (num_buffers_queued == OAL_BUFFERS && !num_buffers_processed)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Remove the Buffer from the Queue.
    if (num_buffers_processed)
    {
      std::array<ALuint, OAL_BUFFERS> unqueued_buffer_ids;
      alSourceUnqueueBuffers(source, num_buffers_processed, unqueued_buffer_ids.data());
      err = CheckALError("unqueuing buffers");

      num_buffers_queued -= num_buffers_processed;
    }

    unsigned int min_frames = frames_per_buffer;

    if (use_surround)
    {
      std::array<float, OAL_MAX_FRAMES * SURROUND_CHANNELS> dpl2;
      u32 rendered_frames = m_mixer->MixSurround(dpl2.data(), min_frames);

      if (rendered_frames < min_frames)
        continue;

      if (float32_capable)
      {
        alBufferData(buffers[next_buffer], AL_FORMAT_71CHN32, dpl2.data(),
                     rendered_frames * FRAME_SURROUND_FLOAT, frequency);
      }
      else if (fixed32_capable)
      {
        std::array<int, OAL_MAX_FRAMES * SURROUND_CHANNELS> surround_int32;

        for (u32 i = 0; i < rendered_frames * SURROUND_CHANNELS; ++i)
        {
          surround_int32[i] = static_cast<int>(dpl2[i] * (INT64_C(1) << 31));
        }

        alBufferData(buffers[next_buffer], AL_FORMAT_71CHN32, surround_int32.data(),
                     rendered_frames * FRAME_SURROUND_INT32, frequency);
      }
      else
      {
        std::array<short, OAL_MAX_FRAMES * SURROUND_CHANNELS> surround_short;

        for (u32 i = 0; i < rendered_frames * SURROUND_CHANNELS; ++i)
        {
          surround_short[i] = static_cast<int>(dpl2[i] * (1 << 15));
        }

        alBufferData(buffers[next_buffer], AL_FORMAT_71CHN16, surround_short.data(),
                     rendered_frames * FRAME_SURROUND_SHORT, frequency);
      }

      err = CheckALError("buffering data");
      if (err == AL_INVALID_ENUM)
      {
        // 5.1 is not supported by the host, fallback to stereo
        WARN_LOG(AUDIO,
                 "Unable to set 5.1 surround mode.  Updating OpenAL Soft might fix this issue.");
        use_surround = false;
      }
    }
    else
    {
      u32 rendered_frames = m_mixer->Mix(realtime_buffer.data(), min_frames);

      if (!rendered_frames)
        continue;

      alBufferData(buffers[next_buffer], AL_FORMAT_STEREO16, realtime_buffer.data(),
                   rendered_frames * FRAME_STEREO_SHORT, frequency);
    }

    alSourceQueueBuffers(source, 1, &buffers[next_buffer]);
    err = CheckALError("queuing buffers");

    num_buffers_queued++;
    next_buffer = (next_buffer + 1) % OAL_BUFFERS;

    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
    {
      // Buffer underrun occurred, resume playback
      alSourcePlay(source);
      err = CheckALError("occurred resuming playback");
    }
  }
}

#endif  // HAVE_OPENAL

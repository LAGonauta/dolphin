// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#ifdef _WIN32

#include <windows.h>
#include <climits>
#include <cstring>
#include <thread>

#include "AudioCommon/OpenALStream.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"

static HMODULE s_openal_dll = nullptr;

#define OPENAL_API_VISIT(X)                                                                        \
  X(alBufferData)                                                                                  \
  X(alcCloseDevice)                                                                                \
  X(alcCreateContext)                                                                              \
  X(alcDestroyContext)                                                                             \
  X(alcGetContextsDevice)                                                                          \
  X(alcGetCurrentContext)                                                                          \
  X(alcGetString)                                                                                  \
  X(alcIsExtensionPresent)                                                                         \
  X(alcMakeContextCurrent)                                                                         \
  X(alcOpenDevice)                                                                                 \
  X(alDeleteBuffers)                                                                               \
  X(alDeleteSources)                                                                               \
  X(alGenBuffers)                                                                                  \
  X(alGenSources)                                                                                  \
  X(alGetError)                                                                                    \
  X(alGetSourcei)                                                                                  \
  X(alGetString)                                                                                   \
  X(alIsExtensionPresent)                                                                          \
  X(alSourcef)                                                                                     \
  X(alSourcei)                                                                                     \
  X(alSourcePlay)                                                                                  \
  X(alSourcePlayv)                                                                                 \
  X(alSourceQueueBuffers)                                                                          \
  X(alSourceStop)                                                                                  \
  X(alSourceStopv)                                                                                 \
  X(alSourceUnqueueBuffers)                                                                        \
  X(alGetEnumValue)                                                                                \
  X(alGetProcAddress)

// Create func_t function pointer type and declare a nullptr-initialized static variable of that
// type named "pfunc".
#define DYN_FUNC_DECLARE(func)                                                                     \
  typedef decltype(&func) func##_t;                                                                \
  static func##_t p##func = nullptr;

// Attempt to load the function from the given module handle.
#define OPENAL_FUNC_LOAD(func)                                                                     \
  p##func = (func##_t)::GetProcAddress(s_openal_dll, #func);                                       \
  if (!p##func)                                                                                    \
  {                                                                                                \
    return false;                                                                                  \
  }

OPENAL_API_VISIT(DYN_FUNC_DECLARE);

static bool InitFunctions()
{
  OPENAL_API_VISIT(OPENAL_FUNC_LOAD);
  return true;
}

static bool InitLibrary()
{
  if (s_openal_dll)
    return true;

  s_openal_dll = ::LoadLibrary(TEXT("openal32.dll"));
  if (!s_openal_dll)
    return false;

  if (!InitFunctions())
  {
    ::FreeLibrary(s_openal_dll);
    s_openal_dll = nullptr;
    return false;
  }

  return true;
}

bool OpenALStream::isValid()
{
  return InitLibrary();
}

//
// AyuanX: Spec says OpenAL1.1 is thread safe already
//
bool OpenALStream::Start()
{
  if (!palcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT"))
  {
    PanicAlertT("OpenAL: can't find sound devices");
    return false;
  }

  const char* default_device_dame = palcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
  INFO_LOG(AUDIO, "Found OpenAL device %s", default_device_dame);

  ALCdevice* device = palcOpenDevice(default_device_dame);
  if (!device)
  {
    PanicAlertT("OpenAL: can't open device %s", default_device_dame);
    return false;
  }

  ALCcontext* context = palcCreateContext(device, nullptr);
  if (!context)
  {
    palcCloseDevice(device);
    PanicAlertT("OpenAL: can't create context for device %s", default_device_dame);
    return false;
  }

  palcMakeContextCurrent(context);
  m_run_thread.Set();
  m_thread = std::thread(&OpenALStream::SoundLoop, this);
  return true;
}

void OpenALStream::Stop()
{
  m_run_thread.Clear();
  // kick the thread if it's waiting
  m_sound_sync_event.Set();

  m_thread.join();

  for (int i = 0; i < OAL_NUM_SOURCES; ++i)
  {
    palSourceStop(m_sources[i]);
    palSourcei(m_sources[i], AL_BUFFER, 0);

    // Clean up buffers and sources
    palDeleteSources(1, &m_sources[i]);
    m_sources[i] = 0;
    palDeleteBuffers(OAL_BUFFERS, m_buffers[i].data());
  }

  ALCcontext* context = palcGetCurrentContext();
  ALCdevice* device = palcGetContextsDevice(context);

  palcMakeContextCurrent(nullptr);
  palcDestroyContext(context);
  palcCloseDevice(device);
}

void OpenALStream::SetVolume(int volume)
{
  m_volume = (float)volume / 100.0f;

  for (int i = 0; i < OAL_NUM_SOURCES; ++i)
  {
    if (m_sources[i])
      palSourcef(m_sources[i], AL_GAIN, m_volume);
  }
}

void OpenALStream::Update()
{
  m_sound_sync_event.Set();
}

static ALenum CheckALError(const char* desc)
{
  ALenum err = palGetError();

  if (err != AL_NO_ERROR)
  {
    ERROR_LOG(AUDIO, "Error %s: %08x %s", desc, err, palGetString(err));
  }

  return err;
}

void OpenALStream::Clear(bool mute)
{
  m_muted = mute;

  if (m_muted)
  {
    palSourceStopv(OAL_NUM_SOURCES, m_sources.data());
    CheckALError("stopping sources playback");
  }
  else
  {
    palSourcePlayv(OAL_NUM_SOURCES, m_sources.data());
    CheckALError("starting sources playback");
  }
}

static bool IsCreativeXFi()
{
  return strstr(palGetString(AL_RENDERER), "X-Fi") != nullptr;
}

void OpenALStream::SoundLoop()
{
  Common::SetCurrentThreadName("Audio thread - openal");

  bool float32_capable = palIsExtensionPresent("AL_EXT_float32") != 0;
  bool surround_capable = palIsExtensionPresent("AL_EXT_MCFORMATS") || IsCreativeXFi();
  bool use_surround = SConfig::GetInstance().bDPL2Decoder && surround_capable;
  bool using_HLE = SConfig::GetInstance().bDSPHLE;

  // As there is no extension to check for 32-bit fixed point support
  // and we know that only a X-Fi with hardware OpenAL supports it,
  // we just check if one is being used.
  bool fixed32_capable = IsCreativeXFi();

  std::array<u32, OAL_NUM_SOURCES> frequency;
  frequency[0] = m_mixer->GetDMASampleRate();
  frequency[1] = m_mixer->GetStreamingSampleRate();
  frequency[2] = m_mixer->GetWiiMoteSampleRate();

  // Clear error state before querying or else we get false positives.
  ALenum err = palGetError();
  std::array<u32, OAL_NUM_SOURCES> frames_per_buffer;
  for (int i = 0; i < OAL_NUM_SOURCES; ++i)
  {
    // calculate latency (samples) per buffer
    if (SConfig::GetInstance().iLatency > 0)
    {
      frames_per_buffer[i] = frequency[i] / 1000 * SConfig::GetInstance().iLatency / OAL_BUFFERS;
    }
    else
    {
      frames_per_buffer[i] = frequency[i] / 1000 * 1 / OAL_BUFFERS;
    }


    if (frames_per_buffer[i] > OAL_MAX_FRAMES)
    {
      frames_per_buffer[i] = OAL_MAX_FRAMES;
    }

    // DPL2 needs a minimum number of samples to work (FWRDURATION) (on any sample rate, or needs
    // only 5ms of data? WiiMote should not use DPL2)
    if (use_surround && frames_per_buffer[i] < 240 && i != 2)
    {
      frames_per_buffer[i] = 240;
    }

    INFO_LOG(AUDIO, "Using %d buffers, each with %d audio frames for a total of %d.", OAL_BUFFERS,
      frames_per_buffer[i], frames_per_buffer[i] * OAL_BUFFERS);

    // Should we make these larger just in case the mixer ever sends more samples
    // than what we request?
    m_realtime_buffers[i].resize(frames_per_buffer[i] * STEREO_CHANNELS);
    m_sources[i] = 0;

    // Generate some AL Buffers for streaming
    palGenBuffers(OAL_BUFFERS, (ALuint*)m_buffers[i].data());
    err = CheckALError("generating buffers");

    // Force disable X-RAM, we do not want it for streaming sources
    if (palIsExtensionPresent("EAX-RAM"))
    {
      EAXSetBufferMode eaxSetBufferMode;
      eaxSetBufferMode = (EAXSetBufferMode)palGetProcAddress("EAXSetBufferMode");
      bool status = eaxSetBufferMode(OAL_BUFFERS, m_buffers[i].data(), palGetEnumValue("AL_STORAGE_ACCESSIBLE"));
      if (status == false)
        ERROR_LOG(AUDIO, "Error setting-up X-RAM mode.");
    }

    // Generate a Source to playback the Buffers
    palGenSources(1, &m_sources[i]);
    err = CheckALError("generating sources");

    // Set the default sound volume as saved in the config file.
    palSourcef(m_sources[i], AL_GAIN, m_volume);
  }
  // TODO: Error handling
  // ALenum err = alGetError();

  std::array<size_t, OAL_NUM_SOURCES> next_buffer = {0, 0, 0};
  std::array<size_t, OAL_NUM_SOURCES> num_buffers_queued = {0, 0, 0};
  std::array<ALint, OAL_NUM_SOURCES> state = {0, 0, 0};

  while (m_run_thread.IsSet())
  {
    // Use mixing only for stereo
    if (use_surround)
    {
      float rate = m_mixer->GetCurrentSpeed();
      // Place a lower limit of 10% speed.  When a game boots up, there will be
      // many silence samples.  These do not need to be timestretched.
      if (SConfig::GetInstance().m_audio_stretch)
      {
        palSourcef(m_sources[0], AL_PITCH, 1.0f);
      }
      else if (rate > 0.10)
      {
        palSourcef(m_sources[0], AL_PITCH, rate);
      }

      // Block until we have a free buffer
      int num_buffers_processed;
      palGetSourcei(m_sources[0], AL_BUFFERS_PROCESSED, &num_buffers_processed);
      if (num_buffers_queued[0] == OAL_BUFFERS && !num_buffers_processed)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      // Remove the Buffer from the Queue.
      if (num_buffers_processed)
      {
        std::array<ALuint, OAL_BUFFERS> unqueued_buffer_ids;
        palSourceUnqueueBuffers(m_sources[0], num_buffers_processed, unqueued_buffer_ids.data());
        err = CheckALError("unqueuing buffers");

        num_buffers_queued[0] -= num_buffers_processed;
      }

      unsigned int min_frames = frames_per_buffer[0];
      std::array<float, OAL_MAX_FRAMES * SURROUND_CHANNELS> dpl2;
      u32 rendered_frames = m_mixer->MixSurround(dpl2.data(), min_frames);

      if (rendered_frames < min_frames)
        continue;

      // zero-out the subwoofer channel - DPL2Decode generates a pretty
      // good 5.0 but not a good 5.1 output.  Sadly there is not a 5.0
      // AL_FORMAT_50CHN32 to make this super-explicit.
      // DPL2Decode output: LEFTFRONT, RIGHTFRONT, CENTREFRONT, (sub), LEFTREAR, RIGHTREAR
      for (u32 i = 0; i < rendered_frames; ++i)
      {
        dpl2[i * SURROUND_CHANNELS + 3 /*sub/lfe*/] = 0.0f;
      }

      if (float32_capable)
      {
        palBufferData(m_buffers[0][next_buffer[0]], AL_FORMAT_51CHN32, dpl2.data(),
          rendered_frames * FRAME_SURROUND_FLOAT, frequency[0]);
      }
      else if (fixed32_capable)
      {
        std::array<int, OAL_MAX_FRAMES * SURROUND_CHANNELS> surround_int32;

        for (u32 i = 0; i < rendered_frames * SURROUND_CHANNELS; ++i)
        {
          // For some reason the ffdshow's DPL2 decoder outputs samples bigger than 1.
          // Most are close to 2.5 and some go up to 8. Hard clamping here, we need to
          // fix the decoder or implement a limiter.
          dpl2[i] = dpl2[i] * (INT64_C(1) << 31);
          if (dpl2[i] > INT_MAX)
            surround_int32[i] = INT_MAX;
          else if (dpl2[i] < INT_MIN)
            surround_int32[i] = INT_MIN;
          else
            surround_int32[i] = static_cast<int>(dpl2[i]);
        }

        palBufferData(m_buffers[0][next_buffer[0]], AL_FORMAT_51CHN32, surround_int32.data(),
          rendered_frames * FRAME_SURROUND_INT32, frequency[0]);
      }
      else
      {
        std::array<short, OAL_MAX_FRAMES * SURROUND_CHANNELS> surround_short;

        for (u32 i = 0; i < rendered_frames * SURROUND_CHANNELS; ++i)
        {
          dpl2[i] = dpl2[i] * (1 << 15);
          if (dpl2[i] > SHRT_MAX)
            surround_short[i] = SHRT_MAX;
          else if (dpl2[i] < SHRT_MIN)
            surround_short[i] = SHRT_MIN;
          else
            surround_short[i] = static_cast<int>(dpl2[i]);
        }

        palBufferData(m_buffers[0][next_buffer[0]], AL_FORMAT_51CHN16, surround_short.data(),
          rendered_frames * FRAME_SURROUND_SHORT, frequency[0]);
      }

      err = CheckALError("buffering data");
      if (err == AL_INVALID_ENUM)
      {
        // 5.1 is not supported by the host, fallback to stereo
        WARN_LOG(AUDIO,
          "Unable to set 5.1 surround mode.  Updating OpenAL Soft might fix this issue.");
        use_surround = false;
      }

      palSourceQueueBuffers(m_sources[0], 1, &m_buffers[0][next_buffer[0]]);
      err = CheckALError("queuing buffers");

      num_buffers_queued[0]++;
      next_buffer[0] = (next_buffer[0] + 1) % OAL_BUFFERS;

      palGetSourcei(m_sources[0], AL_SOURCE_STATE, &state[0]);
      if (state[0] != AL_PLAYING)
      {
        // Buffer underrun occurred, resume playback
        palSourcePlay(m_sources[0]);
        err = CheckALError("occurred resuming playback");
      }
    }
    else
    {
      for (int nsource = 0; nsource < OAL_NUM_SOURCES; ++nsource)
      {
        // Used to change the parameters when the frequency changes. Ugly, need to make it better
        bool changed = false;
        switch (nsource)
        {
        case 0:
          if (frequency[nsource] != m_mixer->GetDMASampleRate())
          {
            changed = true;
            frequency[nsource] = m_mixer->GetDMASampleRate();
          }
          break;
        case 1:
          if (frequency[nsource] != m_mixer->GetStreamingSampleRate())
          {
            changed = true;
            frequency[nsource] = m_mixer->GetStreamingSampleRate();
          }
          break;
        case 2:
          if (frequency[nsource] != m_mixer->GetWiiMoteSampleRate())
          {
            changed = true;
            frequency[nsource] = m_mixer->GetWiiMoteSampleRate();
          }
          break;
        }

        if (changed)
        {
          if (SConfig::GetInstance().iLatency > 10)
          {
            frames_per_buffer[nsource] =
              frequency[nsource] / 1000 * SConfig::GetInstance().iLatency / OAL_BUFFERS;
          }
          else
          {
            frames_per_buffer[nsource] = frequency[nsource] / 1000 * 1 / OAL_BUFFERS;
          }

          // Unbind buffers
          palSourceStop(m_sources[nsource]);
          palSourcei(m_sources[nsource], AL_BUFFER, 0);

          // Clean up queues
          num_buffers_queued[nsource] = 0;
          next_buffer[nsource] = 0;

          // Resize arrays
          m_realtime_buffers[nsource].resize(frames_per_buffer[nsource] * STEREO_CHANNELS);
          continue;
        }

        float rate = m_mixer->GetCurrentSpeed();
        // Place a lower limit of 10% speed.  When a game boots up, there will be
        // many silence samples.  These do not need to be timestretched.
        if (SConfig::GetInstance().m_audio_stretch)
        {
          palSourcef(m_sources[nsource], AL_PITCH, 1.0f);
        }
        else if (rate > 0.10)
        {
          palSourcef(m_sources[nsource], AL_PITCH, rate);
        }

        // Block until we have a free buffer
        int num_buffers_processed;
        palGetSourcei(m_sources[nsource], AL_BUFFERS_PROCESSED, &num_buffers_processed);
        if (num_buffers_queued[nsource] == OAL_BUFFERS && !num_buffers_processed)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }

        // Remove the Buffer from the Queue.
        if (num_buffers_processed)
        {
          std::array<ALuint, OAL_BUFFERS> unqueued_buffer_ids;
          palSourceUnqueueBuffers(m_sources[nsource], num_buffers_processed, unqueued_buffer_ids.data());
          err = CheckALError("unqueuing buffers");

          num_buffers_queued[nsource] -= num_buffers_processed;
        }

        unsigned int rendered_frames = 0;
        switch (nsource)
        {
        case 0:
          rendered_frames = m_mixer->MixDMA(m_realtime_buffers[nsource].data(), frames_per_buffer[nsource], false, true);
          break;
        case 1:
          rendered_frames = m_mixer->MixStreaming(m_realtime_buffers[nsource].data(), frames_per_buffer[nsource], false,
            true);
          break;
        case 2:
          if (using_HLE)
          {
            rendered_frames = m_mixer->MixWiiMote(m_realtime_buffers[nsource].data(), frames_per_buffer[nsource], false,
              true);
          }
          break;
        }

        if (!rendered_frames)
          continue;

        // WiiMote data is too small for the X-Fi on low latency values, convert it to 32-bit so it gets larger
        if (nsource == 2 && fixed32_capable)
        {
          std::array<long, OAL_MAX_FRAMES * STEREO_CHANNELS> wiimote_audio_data;
          long ratio = std::numeric_limits<long>::max() / std::numeric_limits<short>::max();
          for (int i = 0, total_samples = rendered_frames * STEREO_CHANNELS; i < total_samples; ++i)
          {
            wiimote_audio_data[i] = m_realtime_buffers[nsource][i] * ratio;
          }

          palBufferData(m_buffers[nsource][next_buffer[nsource]], AL_FORMAT_STEREO32,
            wiimote_audio_data.data(), rendered_frames * FRAME_STEREO_SHORT * 2,
            frequency[nsource]);
        }
        else
        {
          palBufferData(m_buffers[nsource][next_buffer[nsource]], AL_FORMAT_STEREO16,
            m_realtime_buffers[nsource].data(), rendered_frames * FRAME_STEREO_SHORT,
            frequency[nsource]);
        }

        palSourceQueueBuffers(m_sources[nsource], 1, &m_buffers[nsource][next_buffer[nsource]]);
        err = CheckALError("queuing buffers");

        num_buffers_queued[nsource]++;
        next_buffer[nsource] = (next_buffer[nsource] + 1) % OAL_BUFFERS;

        palGetSourcei(m_sources[nsource], AL_SOURCE_STATE, &state[nsource]);
        if (state[nsource] != AL_PLAYING)
        {
          // Buffer underrun occurred, resume playback
          palSourcePlay(m_sources[nsource]);
          err = CheckALError("occurred resuming playback");
        }
      }
    }
  }
}

#endif  // _WIN32

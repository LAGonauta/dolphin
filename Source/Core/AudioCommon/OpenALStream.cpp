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
#include "FreeSurround/FreeSurroundDecoder.h"

#if defined HAVE_OPENAL && HAVE_OPENAL

#ifdef _WIN32
#pragma comment(lib, "openal32.lib")
#endif

static DPL2FSDecoder fsdecoder;

//
// AyuanX: Spec says OpenAL1.1 is thread safe already
//
bool OpenALStream::Start()
{
  m_run_thread.Set();
  bool bReturn = false;

  ALDeviceList pDeviceList;
  if (pDeviceList.GetNumDevices())
  {
    char* defDevName = pDeviceList.GetDeviceName(pDeviceList.GetDefaultDevice());

    INFO_LOG(AUDIO, "Found OpenAL device %s", defDevName);

    ALCdevice* pDevice = alcOpenDevice(defDevName);
    if (pDevice)
    {
      ALCcontext* pContext = alcCreateContext(pDevice, nullptr);
      if (pContext)
      {
        // Used to determine an appropriate period size (2x period = total buffer size)
        // ALCint refresh;
        // alcGetIntegerv(pDevice, ALC_REFRESH, 1, &refresh);
        // period_size_in_millisec = 1000 / refresh;

        alcMakeContextCurrent(pContext);
        thread = std::thread(&OpenALStream::SoundLoop, this);
        bReturn = true;
      }
      else
      {
        alcCloseDevice(pDevice);
        PanicAlertT("OpenAL: can't create context for device %s", defDevName);
      }
    }
    else
    {
      PanicAlertT("OpenAL: can't open device %s", defDevName);
    }
  }
  else
  {
    PanicAlertT("OpenAL: can't find sound devices");
  }

  // Initialize the decoder.
  fsdecoder.Init();

  return bReturn;
}

void OpenALStream::Stop()
{
  m_run_thread.Clear();
  // kick the thread if it's waiting
  soundSyncEvent.Set();

  thread.join();

  for (int i = 0; i < SFX_MAX_SOURCES; ++i)
  {
	  alSourceStop(sources[i]);
	  alSourcei(sources[i], AL_BUFFER, 0);

	  // Clean up buffers and sources
	  alDeleteSources(1, &sources[i]);
	  sources[i] = 0;
	  alDeleteBuffers(numBuffers, buffers[i].data());
  }

  ALCcontext* pContext = alcGetCurrentContext();
  ALCdevice* pDevice = alcGetContextsDevice(pContext);

  alcMakeContextCurrent(nullptr);
  alcDestroyContext(pContext);
  alcCloseDevice(pDevice);
}

void OpenALStream::SetVolume(int volume)
{
  global_volume = (float)volume / 100.0f;

  if (use_full_HRTF)
  {
    for (int i = 0; i < SFX_MAX_SOURCES; ++i)
    {
      if (sources[i])
        alSourcef(sources[i], AL_GAIN, global_volume);
    }
  }
  else
  {
    if (sources[0])
      alSourcef(sources[0], AL_GAIN, global_volume);
  }
}

void OpenALStream::Update()
{
  soundSyncEvent.Set();
}

void OpenALStream::Clear(bool mute)
{
  m_muted = mute;

  if (m_muted)
  {
      if (use_full_HRTF)
      {
        for (int i = 0; i < SFX_MAX_SOURCES; ++i)
        {
          alSourceStop(sources[i]);
        }
      }
      else
      {
        alSourceStop(sources[0]);
      }
  }
  else
  {
      if (use_full_HRTF)
      {
        for (int i = 0; i < SFX_MAX_SOURCES; ++i)
        {
          alSourcePlay(sources[i]);
        }
      }
      else
      {
        alSourcePlay(sources[0]);
      }
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

void OpenALStream::SoundLoop()
{
  Common::SetCurrentThreadName("Audio thread - openal");

  bool surround_capable = SConfig::GetInstance().bDPL2Decoder;
  bool float32_capable = false;
  bool fixed32_capable = false;
  use_full_HRTF = true;

#if defined(__APPLE__)
  surround_capable = false;
#endif

  u32 ulFrequency = m_mixer->GetSampleRate();
  numBuffers = SConfig::GetInstance().iLatency + 2;  // OpenAL requires a minimum of two buffers

  if (alIsExtensionPresent("AL_EXT_float32"))
    float32_capable = true;

  // As there is no extension to check for 32-bit fixed point support
  // and we know that only a X-Fi with hardware OpenAL supports it,
  // we just check if one is being used.
  if (strstr(alGetString(AL_RENDERER), "X-Fi"))
    fixed32_capable = true;

  // Clear error state before querying or else we get false positives.
  ALenum err = alGetError();

  // Generate some AL Buffers for streaming
  for (int i = 0; i < SFX_MAX_SOURCES; ++i)
  {
    alGenBuffers(numBuffers, buffers[i].data());
  }
  err = CheckALError("generating buffers");

  // Generate a Source to playback the Buffers
  if (use_full_HRTF)
  {
    alGenSources(SFX_MAX_SOURCES, sources.data());
  }
  else
  {
    alGenSources(1, sources.data());
  }
  err = CheckALError("generating sources");

  // Set the default sound volume as saved in the config file.

  if (use_full_HRTF)
  {
    for (int i = 0; i < SFX_MAX_SOURCES; ++i)
    {
      alSourcef(sources[i], AL_GAIN, global_volume);
    }
	float scale = 1.0f;
	// Front left
	alSource3f(sources[0], AL_POSITION, scale * -1.0f, 0.0f, scale * -1.0f);

	// Front center left
	alSource3f(sources[1], AL_POSITION, scale * -0.5f, 0.0f, scale * -1.0f);

	// Front center
	alSource3f(sources[2], AL_POSITION, scale * 0.0f, 0.0f, scale * -1.0f);

	// Front center right
	alSource3f(sources[3], AL_POSITION, scale * 0.5f, 0.0f, scale * -1.0f);

	// Front right
	alSource3f(sources[4], AL_POSITION, scale * 1.0f, 0.0f, scale * -1.0f);

	// Side front left
	alSource3f(sources[5], AL_POSITION, scale * -1.0f, 0.0f, scale * -0.5f);

	// Side front right
	alSource3f(sources[6], AL_POSITION, scale * 1.0f, 0.0f, scale * -0.5f);

	// Side center left
	alSource3f(sources[7], AL_POSITION, scale * -1.0f, 0.0f, scale * 0.0f);

	// Side center right
	alSource3f(sources[8], AL_POSITION, scale * 1.0f, 0.0f, scale * 0.0f);

	// Side back left
	alSource3f(sources[9], AL_POSITION, scale * -1.0f, 0.0f, scale * 0.5f);

	// Side back right
	alSource3f(sources[10], AL_POSITION, scale * 1.0f, 0.0f, scale * 0.5f);

	// Back left
	alSource3f(sources[11], AL_POSITION, scale * -1.0f, 0.0f, scale * 1.0f);

	// Back center left
	alSource3f(sources[12], AL_POSITION, scale * -0.5f, 0.0f, scale * 1.0f);

	// Back center
	alSource3f(sources[13], AL_POSITION, scale * 0.0f, 0.0f, scale * 1.0f);

	// Back center right
	alSource3f(sources[14], AL_POSITION, scale * 0.5f, 0.0f, scale * 1.0f);

	// Back right
	alSource3f(sources[15], AL_POSITION, scale * 1.0f, 0.0f, scale * 1.0f);
  }
  else
  {
    alSourcef(sources[0], AL_GAIN, global_volume);
  }

  // TODO: Error handling
  // ALenum err = alGetError();

  unsigned int nextBuffer = 0;
  unsigned int numBuffersQueued = 0;
  ALint iState = 0;

  fsdecoder.set_block_size(OAL_MAX_SAMPLES);
  fsdecoder.set_sample_rate(ulFrequency);

  if (use_full_HRTF)
    fsdecoder.set_channel_setup(cs_16point1);

  while (m_run_thread.IsSet())
  {
    // Block until we have a free buffer
    int numBuffersProcessed;
    alGetSourcei(sources[0], AL_BUFFERS_PROCESSED, &numBuffersProcessed);
    if (numBuffers == numBuffersQueued && !numBuffersProcessed)
    {
      // soundSyncEvent.Wait();
      continue;
    }

    // Remove the Buffer from the Queue.
    if (numBuffersProcessed)
    {
      ALuint unqueuedBufferIds[OAL_MAX_BUFFERS];
      if (use_full_HRTF)
      {
        for (int i = 0; i < SFX_MAX_SOURCES; ++i)
          alSourceUnqueueBuffers(sources[i], numBuffersProcessed, unqueuedBufferIds);
      }
      else
      {
        alSourceUnqueueBuffers(sources[0], numBuffersProcessed, unqueuedBufferIds);
      }

      err = CheckALError("unqueuing buffers");

      numBuffersQueued -= numBuffersProcessed;
    }

    // FreeSurround DPL2 decoder needs 256 samples minimum
    unsigned int minSamples = surround_capable ? 256 : 0;

    unsigned int numSamples = OAL_MAX_SAMPLES;
    numSamples = m_mixer->Mix(realtime_buffer.data(), numSamples);

    // Convert the samples from short to float
    for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
      sample_buffer[i] = static_cast<float>(realtime_buffer[i]) / INT16_MAX;

    if (numSamples < minSamples)
      continue;

    if (surround_capable)
    {
      float* dpl2_fs = fsdecoder.decode(sample_buffer.data());

      if (use_full_HRTF)
      {
        // multiplying capacity by 3 just to be sure
        std::array<std::array<float, OAL_MAX_SAMPLES * 3>, SFX_MAX_SOURCES> dpl2;

        // Correct channel mapping for OpenAL
        // FreeSurround:
        //  ci_front_left -> ci_front_center_left -> ci_front_center ->
        //  ci_front_center_right -> ci_front_right -> ci_side_front_left ->
        //  ci_side_front_right -> ci_side_center_left ->
        //  ci_side_center_right -> ci_side_back_left -> ci_side_back_right ->
        //  ci_back_left -> ci_back_center_left -> ci_back_center ->
        //  ci_back_center_right -> ci_back_right -> ci_lfe,

        for (u32 i = 0; i < numSamples; ++i)
        {
          // add one to SFX_MAX_SOURCES because of the LFE
          int channels = SFX_MAX_SOURCES + 1;

          // Front left
          dpl2[0][i] = dpl2_fs[i * channels + 0];

          // Front center left
          dpl2[1][i] = dpl2_fs[i * channels + 1];

          // Front center
          dpl2[2][i] = dpl2_fs[i * channels + 2];

          // Front center right
          dpl2[3][i] = dpl2_fs[i * channels + 3];

          // Front right
          dpl2[4][i] = dpl2_fs[i * channels + 4];

          // Side front left
          dpl2[5][i] = dpl2_fs[i * channels + 5];

          // Side front right
          dpl2[6][i] = dpl2_fs[i * channels + 6];

          // Side center left
          dpl2[7][i] = dpl2_fs[i * channels + 7];

          // Side center right
          dpl2[8][i] = dpl2_fs[i * channels + 8];

          // Side back left
          dpl2[9][i] = dpl2_fs[i * channels + 9];

          // Side back right
          dpl2[10][i] = dpl2_fs[i * channels + 10];

          // Back left
          dpl2[11][i] = dpl2_fs[i * channels + 11];

          // Back center left
          dpl2[12][i] = dpl2_fs[i * channels + 12];

          // Back center
          dpl2[13][i] = dpl2_fs[i * channels + 13];

          // Back center right
          dpl2[14][i] = dpl2_fs[i * channels + 14];

          // Back right
          dpl2[15][i] = dpl2_fs[i * channels + 15];

          // LFE
          // no LFE //
        }

        if (float32_capable)
        {
          for (int i = 0; i < SFX_MAX_SOURCES; ++i)
          {
            alBufferData(buffers[i][nextBuffer], alGetEnumValue("AL_MONO32F_SOFT"), dpl2[i].data(),
                         numSamples * SIZE_FLOAT, ulFrequency);
          }
        }
        else if (fixed32_capable)
        {
          for (int i = 0; i < SFX_MAX_SOURCES; ++i)
          {
            std::vector<int> surround_int32(numSamples);

            for (u32 u = 0; u < numSamples; ++u)
            {
              surround_int32[u] = static_cast<int>(dpl2[i][u] * INT32_MAX);
            }

            alBufferData(buffers[i][nextBuffer], alGetEnumValue("AL_FORMAT_MONO32"), surround_int32.data(),
                         numSamples * SIZE_INT32, ulFrequency);
          }
        }
        else
        {
          for (int i = 0; i < SFX_MAX_SOURCES; ++i)
          {
            std::vector<short> surround_short(numSamples);

            for (u32 u = 0; u < numSamples; ++u)
            {
              surround_short[i] = static_cast<int>(dpl2[i][u] * INT16_MAX);
            }

            alBufferData(buffers[i][nextBuffer], AL_FORMAT_MONO16, surround_short.data(),
                         numSamples * SIZE_SHORT, ulFrequency);
          }
        }
      }
      else
      {
        std::vector<float> dpl2(numSamples * SURROUND_CHANNELS);

        // Correct channel mapping for OpenAL
        // FreeSurround:
        // FL | FC | FR | BL | BR | LFE
        // OpenAL:
        // FL | FR | FC | LFE | BL | BR

        for (u32 i = 0; i < numSamples; ++i)
        {
          dpl2[i * SURROUND_CHANNELS + 0 /*LEFTFRONT*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 0 /*LEFTFRONT*/];
          dpl2[i * SURROUND_CHANNELS + 1 /*RIGHTFRONT*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 2 /*RIGHTFRONT*/];
          dpl2[i * SURROUND_CHANNELS + 2 /*CENTREFRONT*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 1 /*CENTREFRONT*/];
          dpl2[i * SURROUND_CHANNELS + 3 /*sub/lfe*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 5 /*sub/lfe*/];
          dpl2[i * SURROUND_CHANNELS + 4 /*LEFTREAR*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 3 /*LEFTREAR*/];
          dpl2[i * SURROUND_CHANNELS + 5 /*RIGHTREAR*/] =
              dpl2_fs[i * SURROUND_CHANNELS + 4 /*RIGHTREAR*/];
        }

        if (float32_capable)
        {
          alBufferData(buffers[0][nextBuffer], AL_FORMAT_51CHN32, dpl2.data(),
                       numSamples * FRAME_SURROUND_FLOAT, ulFrequency);
        }
        else if (fixed32_capable)
        {
          std::vector<int> surround_int32(numSamples * SURROUND_CHANNELS);

          for (u32 i = 0; i < numSamples * SURROUND_CHANNELS; ++i)
          {
            surround_int32[i] = static_cast<int>(dpl2[i] * INT32_MAX);
          }

          alBufferData(buffers[0][nextBuffer], AL_FORMAT_51CHN32, surround_int32.data(),
                       numSamples * FRAME_SURROUND_INT32, ulFrequency);
        }
        else
        {
          std::vector<short> surround_short(numSamples * SURROUND_CHANNELS);

          for (u32 i = 0; i < numSamples * SURROUND_CHANNELS; ++i)
          {
            surround_short[i] = static_cast<int>(dpl2[i] * INT16_MAX);
          }

          alBufferData(buffers[0][nextBuffer], AL_FORMAT_51CHN16, surround_short.data(),
                       numSamples * FRAME_SURROUND_SHORT, ulFrequency);
        }

        err = CheckALError("buffering data");
        if (err == AL_INVALID_ENUM)
        {
          // 5.1 is not supported by the host, fallback to stereo
          WARN_LOG(AUDIO,
                   "Unable to set 5.1 surround mode.  Updating OpenAL Soft might fix this issue.");
          surround_capable = false;
        }
      }
    }
    else
    {
      if (float32_capable)
      {
        alBufferData(buffers[0][nextBuffer], AL_FORMAT_STEREO_FLOAT32, sample_buffer.data(),
                     numSamples * FRAME_STEREO_FLOAT, ulFrequency);

        err = CheckALError("buffering float32 data");
        if (err == AL_INVALID_ENUM)
        {
          float32_capable = false;
        }
      }
      else if (fixed32_capable)
      {
        std::vector<int> stereo_int32(numSamples * STEREO_CHANNELS);
        for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
          stereo_int32[i] = static_cast<int>(sample_buffer[i] * INT32_MAX);

        alBufferData(buffers[0][nextBuffer], AL_FORMAT_STEREO32, stereo_int32.data(),
                     numSamples * FRAME_STEREO_INT32, ulFrequency);
      }
      else
      {
        std::vector<short> stereo(numSamples * STEREO_CHANNELS);
        for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
          stereo[i] = static_cast<short>(sample_buffer[i] * INT16_MAX);

        alBufferData(buffers[0][nextBuffer], AL_FORMAT_STEREO16, stereo.data(),
                     numSamples * FRAME_STEREO_SHORT, ulFrequency);
      }
    }

    if (use_full_HRTF)
    {
      for (int i = 0; i < SFX_MAX_SOURCES; ++i)
      {
        alSourceQueueBuffers(sources[i], 1, &buffers[i][nextBuffer]);
      }
    }
    else
    {
      alSourceQueueBuffers(sources[0], 1, &buffers[0][nextBuffer]);
    }    
    err = CheckALError("queuing buffers");

    numBuffersQueued++;
    nextBuffer = (nextBuffer + 1) % numBuffers;

    alGetSourcei(sources[0], AL_SOURCE_STATE, &iState);
    if (iState != AL_PLAYING)
    {
      // Buffer underrun occurred, resume playback
      if (use_full_HRTF)
      {
        alSourcePlayv(SFX_MAX_SOURCES, sources.data());
      }
      else
      {
        alSourcePlay(sources[0]);
      }
      err = CheckALError("occurred resuming playback");
    }
  }
}

#endif  // HAVE_OPENAL

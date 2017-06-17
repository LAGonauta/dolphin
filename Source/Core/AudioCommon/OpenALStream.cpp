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

  return bReturn;
}

void OpenALStream::Stop()
{
  m_run_thread.Clear();
  // kick the thread if it's waiting
  soundSyncEvent.Set();

  thread.join();

  alSourceStop(uiSource);
  alSourcei(uiSource, AL_BUFFER, 0);

  // Clean up buffers and sources
  alDeleteSources(1, &uiSource);
  uiSource = 0;
  alDeleteBuffers(OAL_BUFFERS, uiBuffers.data());

  ALCcontext* pContext = alcGetCurrentContext();
  ALCdevice* pDevice = alcGetContextsDevice(pContext);

  alcMakeContextCurrent(nullptr);
  alcDestroyContext(pContext);
  alcCloseDevice(pDevice);
}

void OpenALStream::SetVolume(int volume)
{
  fVolume = (float)volume / 100.0f;

  if (uiSource)
    alSourcef(uiSource, AL_GAIN, fVolume);
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
    alSourceStop(uiSource);
  }
  else
  {
    alSourcePlay(uiSource);
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

  u32 ulFrequency = m_mixer->GetSampleRate();

  u32 frames_per_buffer;
  // Can't have zero samples per buffer
  if (SConfig::GetInstance().iLatency > 0)
  {
    frames_per_buffer = ulFrequency / 1000 * SConfig::GetInstance().iLatency / OAL_BUFFERS;
  }
  else
  {
    frames_per_buffer = ulFrequency / 1000 * 1 / OAL_BUFFERS;
  }

  if (frames_per_buffer > OAL_MAX_FRAMES)
  {
    frames_per_buffer = OAL_MAX_FRAMES;
  }

  // DPL2 needs a minimum number of samples to work (FWRDURATION)
  if (use_surround && frames_per_buffer < 240)
  {
    frames_per_buffer = 240;
  }

  INFO_LOG(AUDIO, "Using %d buffers, each with %d audio frames for a total of %d.", OAL_BUFFERS,
           frames_per_buffer, frames_per_buffer * OAL_BUFFERS);

  // Should we make these larger just in case the mixer ever sends more samples
  // than what we request?
  realtimeBuffer.resize(frames_per_buffer * STEREO_CHANNELS);
  sampleBuffer.resize(frames_per_buffer * STEREO_CHANNELS);
  uiSource = 0;

  // Clear error state before querying or else we get false positives.
  ALenum err = alGetError();

  // Generate some AL Buffers for streaming
  alGenBuffers(OAL_BUFFERS, (ALuint*)uiBuffers.data());
  err = CheckALError("generating buffers");

  // Generate a Source to playback the Buffers
  alGenSources(1, &uiSource);
  err = CheckALError("generating sources");

  // Set the default sound volume as saved in the config file.
  alSourcef(uiSource, AL_GAIN, fVolume);

  // TODO: Error handling
  // ALenum err = alGetError();

  unsigned int nextBuffer = 0;
  unsigned int numBuffersQueued = 0;
  ALint iState = 0;

  while (m_run_thread.IsSet())
  {
    // Block until we have a free buffer
    int numBuffersProcessed;
    alGetSourcei(uiSource, AL_BUFFERS_PROCESSED, &numBuffersProcessed);
    if (OAL_BUFFERS == numBuffersQueued && !numBuffersProcessed)
    {
      soundSyncEvent.Wait();
      continue;
    }

    // Remove the Buffer from the Queue.
    if (numBuffersProcessed)
    {
      ALuint unqueuedBufferIds[OAL_BUFFERS];
      alSourceUnqueueBuffers(uiSource, numBuffersProcessed, unqueuedBufferIds);
      err = CheckALError("unqueuing buffers");

      numBuffersQueued -= numBuffersProcessed;
    }

    unsigned int numSamples = frames_per_buffer;

    if (use_surround)
    {
      // DPL2 accepts 240 samples minimum (FWRDURATION)
      unsigned int minSamples = 240;

      float dpl2[OAL_MAX_FRAMES * SURROUND_CHANNELS];
      numSamples = m_mixer->MixSurround(dpl2, numSamples);

      if (numSamples < minSamples)
        continue;

      // zero-out the subwoofer channel - DPL2Decode generates a pretty
      // good 5.0 but not a good 5.1 output.  Sadly there is not a 5.0
      // AL_FORMAT_50CHN32 to make this super-explicit.
      // DPL2Decode output: LEFTFRONT, RIGHTFRONT, CENTREFRONT, (sub), LEFTREAR, RIGHTREAR
      for (u32 i = 0; i < numSamples; ++i)
      {
        dpl2[i * SURROUND_CHANNELS + 3 /*sub/lfe*/] = 0.0f;
      }

      if (float32_capable)
      {
        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_51CHN32, dpl2,
                     numSamples * FRAME_SURROUND_FLOAT, ulFrequency);
      }
      else if (fixed32_capable)
      {
        int surround_int32[OAL_MAX_FRAMES * SURROUND_CHANNELS];

        for (u32 i = 0; i < numSamples * SURROUND_CHANNELS; ++i)
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
            surround_int32[i] = (int)dpl2[i];
        }

        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_51CHN32, surround_int32,
                     numSamples * FRAME_SURROUND_INT32, ulFrequency);
      }
      else
      {
        short surround_short[OAL_MAX_FRAMES * SURROUND_CHANNELS];

        for (u32 i = 0; i < numSamples * SURROUND_CHANNELS; ++i)
        {
          dpl2[i] = dpl2[i] * (1 << 15);
          if (dpl2[i] > SHRT_MAX)
            surround_short[i] = SHRT_MAX;
          else if (dpl2[i] < SHRT_MIN)
            surround_short[i] = SHRT_MIN;
          else
            surround_short[i] = (int)dpl2[i];
        }

        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_51CHN16, surround_short,
                     numSamples * FRAME_SURROUND_SHORT, ulFrequency);
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
      numSamples = m_mixer->Mix(realtimeBuffer.data(), numSamples);

      // Convert the samples from short to float
      for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
        sampleBuffer[i] = static_cast<float>(realtimeBuffer[i]) / (1 << 15);

      if (!numSamples)
        continue;

      if (float32_capable)
      {
        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_STEREO_FLOAT32, sampleBuffer.data(),
                     numSamples * FRAME_STEREO_FLOAT, ulFrequency);

        err = CheckALError("buffering float32 data");
        if (err == AL_INVALID_ENUM)
        {
          float32_capable = false;
        }
      }
      else if (fixed32_capable)
      {
        // Clamping is not necessary here, samples are always between (-1,1)
        int stereo_int32[OAL_MAX_FRAMES * STEREO_CHANNELS];
        for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
          stereo_int32[i] = (int)((float)sampleBuffer[i] * (INT64_C(1) << 31));

        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_STEREO32, stereo_int32,
                     numSamples * FRAME_STEREO_INT32, ulFrequency);
      }
      else
      {
        // Convert the samples from float to short
        short stereo[OAL_MAX_FRAMES * STEREO_CHANNELS];
        for (u32 i = 0; i < numSamples * STEREO_CHANNELS; ++i)
          stereo[i] = (short)((float)sampleBuffer[i] * (1 << 15));

        alBufferData(uiBuffers[nextBuffer], AL_FORMAT_STEREO16, stereo,
                     numSamples * FRAME_STEREO_SHORT, ulFrequency);
      }
    }

    alSourceQueueBuffers(uiSource, 1, &uiBuffers[nextBuffer]);
    err = CheckALError("queuing buffers");

    numBuffersQueued++;
    nextBuffer = (nextBuffer + 1) % OAL_BUFFERS;

    alGetSourcei(uiSource, AL_SOURCE_STATE, &iState);
    if (iState != AL_PLAYING)
    {
      // Buffer underrun occurred, resume playback
      alSourcePlay(uiSource);
      err = CheckALError("occurred resuming playback");
    }
  }
}

#endif  // HAVE_OPENAL

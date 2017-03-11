#pragma once

/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include <list>

#include "cores/IPlayer.h"
#include "threads/Thread.h"
#include "threads/SharedSection.h"
#include "utils/Job.h"

#include "cores/AudioEngine/Interfaces/IAudioCallback.h"
#include "music/tags/MusicInfoTag.h"

#ifdef HAS_DBUS
#include "DBusUtil.h"
#define BLUEZ_SERVICE "org.bluez"

#endif

class IAEStream;

class CFileItem;
class BTPlayer : public IPlayer, public CThread, public IJobCallback
{
  friend class CQueueNextFileJob;
 public:
  BTPlayer(IPlayerCallback& callback);
  virtual ~BTPlayer();

  virtual void RegisterAudioCallback(IAudioCallback* pCallback);
  virtual void UnRegisterAudioCallback();
  virtual bool OpenFile(const CFileItem& file, const CPlayerOptions &options);
  virtual bool QueueNextFile(const CFileItem &file);
  virtual void OnNothingToQueueNotify();
  virtual bool CloseFile(bool reopen = false);
  virtual bool IsPlaying() const;
  virtual void Pause();
  virtual bool IsPaused() const;
  virtual bool HasVideo() const { return false; }
  virtual bool HasAudio() const { return true; }
  virtual bool CanSeek() const { return false; }
  virtual void Seek(bool bPlus = true, bool bLargeStep = false, bool bChapterOverride = false);
  virtual void SeekPercentage(float fPercent = 0.0f) { return; }
  virtual float GetPercentage();
  virtual void SetVolume(float volume) { return; }
  virtual void SetDynamicRangeCompression(long drc) { return; }
  virtual void GetAudioInfo( std::string& strAudioInfo) {}
  virtual void GetVideoInfo( std::string& strVideoInfo) {}
  virtual void GetGeneralInfo( std::string& strVideoInfo) {}
  virtual void ToFFRW(int iSpeed = 0) { return; }
  virtual int GetCacheLevel() const;
  virtual int64_t GetTotalTime();
  virtual void SetSpeed(float iSpeed) override;
  virtual float GetSpeed() override;
  virtual void ShowOSD(bool bOnoff){};
  virtual void DoAudioWork() {};
  virtual void GetAudioStreamInfo(int index, SPlayerAudioStreamInfo &info);
  virtual int64_t GetTime();
  virtual void SeekTime() const { return; }
  virtual bool SkipNext() const { return false; }
  virtual bool IsPassthrough() const { return true; }
  virtual void GetAudioCapabilities(std::vector<int> &audioCaps) {}

  static bool HandlesType(const std::string &type) { return true; }
  

  virtual void OnJobComplete(unsigned int jobID, bool success, CJob *job);
#ifdef HAS_DBUS
  virtual void logDBusError();
  virtual void updateA2DPInfo(std::string& destination_path);
#endif

    struct
    {
      char         m_codec[21];
      int64_t      m_time;
      int64_t      m_totalTime;
      int          m_channelCount;
      int          m_bitsPerSample;
      int          m_sampleRate;
      int          m_audioBitrate;
      int          m_cacheLevel;
      bool         m_canSeek;
    } m_playerGUIData;

 protected:
    virtual void OnStartup() {}
    virtual void Process();
    virtual void OnExit();

 private:

    bool                      m_signalSpeedChange;   /* true if OnPlaybackSpeedChange needs to be called */
    int                       m_playbackSpeed;       /* the playback speed (1 = normal) */
    bool                      m_isPlaying;
    bool                      m_isPaused;
    bool                      m_isFinished;          /* if there are no more songs in the queue */
    unsigned int              m_defaultCrossfadeMS;  /* how long the default crossfade is in ms */
    unsigned int              m_upcomingCrossfadeMS; /* how long the upcoming crossfade is in ms */
    CEvent                    m_startEvent;          /* event for playback start */
    IAudioCallback*           m_audioCallback;       /* the viz audio callback */

    CFileItem*                m_FileItem;            /* our queued file or current file if no file is queued */
    MUSIC_INFO::CMusicInfoTag m_tag;
    CCriticalSection          m_streamsLock;         /* lock for the stream list */
    int                       m_jobCounter;
    CEvent                    m_jobEvent;
    bool                      m_continueStream;
    bool                      m_bStop;
    int64_t                   GetTotalTime64();
    int64_t                   GetTimeInternal();
    bool                      haveTrackDetailsChanged(CVariant track, MUSIC_INFO::CMusicInfoTag &tag);
    int                       m_numberOfTracks =0;
#ifdef HAS_DBUS
   std::string                m_dbus_path;
   unsigned int               m_dbus_errors;
#endif
};

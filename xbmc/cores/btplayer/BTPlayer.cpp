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

#include "BTPlayer.h"
#include "FileItem.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "music/tags/MusicInfoTag.h"
#include "utils/log.h"
#include "utils/JobManager.h"
#include "messaging/ApplicationMessenger.h"
#include "cores/AudioEngine/AEFactory.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"
#include "cores/AudioEngine/Interfaces/IAudioCallback.h"

#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include <unistd.h>

using namespace MUSIC_INFO;
using namespace KODI::MESSAGING;

class CQueueNextFileJob : public CJob
{
  CFileItem m_item;
  BTPlayer &m_player;

public:
  CQueueNextFileJob(const CFileItem& item, BTPlayer &player)
    : m_item(item), m_player(player) {}
  virtual       ~CQueueNextFileJob() {}
  virtual bool  DoWork()
  {
    return true;
  }
};

// BTPlayer: Bluetooth Player
// A dummy player that lets Pulse and Kodi be friends without hassle


BTPlayer::BTPlayer(IPlayerCallback& callback) :
  IPlayer              (callback),
  CThread              ("BTPlayer"),
  m_signalSpeedChange  (false),
  m_playbackSpeed      (1    ),
  m_isPlaying          (false),
  m_isPaused           (false),
  m_isFinished         (false),
  m_defaultCrossfadeMS (0),
  m_upcomingCrossfadeMS(0),
  m_audioCallback      (NULL ),
  m_FileItem           (new CFileItem()),
  m_jobCounter         (0),
  m_continueStream     (false),
  m_bStop              (false)
{
  memset(&m_playerGUIData, 0, sizeof(m_playerGUIData));
}

BTPlayer::~BTPlayer()
{
  CloseFile();
  delete m_FileItem;
}

bool BTPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  StopThread();
  m_isPaused = false;
  CLog::Log (LOGINFO, "DBus Path : %s", file.GetPath().c_str());
#ifdef HAS_DBUS
  m_dbus_path = file.GetPath();
  m_dbus_errors = 0;
#endif
  CSingleLock lock(m_streamsLock);
  /* Suspend AE temporarily so exclusive or hog-mode sinks */
  /* don't block external player's access to audio device  */
  CAEFactory::Suspend ();
  // wait for AE has completed suspended
  XbmcThreads::EndTime timer (2000);
  while (!timer.IsTimePast () && !CAEFactory::IsSuspended ())
  {
    Sleep (50);
  }
  if (timer.IsTimePast ())
  {
    CLog::Log (LOGERROR, "%s: AudioEngine did not suspend", __FUNCTION__);
  }
  lock.Leave();
  if (!IsRunning())
    Create();

  /* trigger playback start */
  m_isPlaying = true;
  m_startEvent.Set();
  return true;
}

bool BTPlayer::QueueNextFile(const CFileItem &file)
{
  {
    CSingleLock lock(m_streamsLock);
    m_jobCounter++;
  }
  return true;
}

bool BTPlayer::CloseFile(bool reopen)
{
  CLog::Log(LOGDEBUG, "BTPlayer::CloseFile");
  m_bStop = true;
#ifdef HAS_DBUS
  m_dbus_path.clear();
#endif
  StopThread(true);
  CSingleLock lock(m_streamsLock);
  while (m_jobCounter > 0)
  {
    lock.Leave();
    m_jobEvent.WaitMSec(100);
    lock.Enter();
  }
  /* Resume AE processing of XBMC native audio */
  if (!CAEFactory::Resume ())
  {
    CLog::Log (LOGFATAL, "%s: Failed to restart AudioEngine", __FUNCTION__);
  }	
  return true;
}

#ifdef HAS_DBUS
void BTPlayer::logDBusError()
{
  m_dbus_errors++;
  if (m_dbus_errors > 50)
    {
      CLog::Log (LOGFATAL, "Too Many DBUS errors Closing BTPlayer");
      m_bStop = true;
    }
}

bool BTPlayer::haveTrackDetailsChanged(CVariant track, MUSIC_INFO::CMusicInfoTag &tag)
{
  bool trackDetailsChanged = false;
  if (!tag.GetArtist().empty())
    {
      if (tag.GetArtist().front().compare(track["Artist"].asString()) != 0)
	{
	  trackDetailsChanged = true;
	}
  }
  if (tag.GetTitle().compare(track["Title"].asString()) != 0)
  {
    trackDetailsChanged = true;
  }
  if (tag.GetAlbum().compare(track["Album"].asString()) != 0)
  {
    trackDetailsChanged = true;
  }
  if (tag.GetTrackNumber() != track["TrackNumber"].asInteger())
  {
    trackDetailsChanged = true;
  }
  return trackDetailsChanged;
}

void BTPlayer::updateA2DPInfo(std::string& destination_path)
{
  bool trackDetailsChanged = false;
  int numberOfTracks = m_numberOfTracks;
  int trackNumber = 0;
  // get details from bluez
  CVariant track = CDBusUtil::GetVariant("org.bluez",
                                         destination_path.c_str(),
					 "org.bluez.MediaPlayer1",
					 "Track");
  if (track == NULL)
    {
      logDBusError();
    }
  else
    {
      trackDetailsChanged = haveTrackDetailsChanged(track, m_tag);
      if (trackDetailsChanged){
	m_tag.SetArtist(track["Artist"].asString());
	m_tag.SetTitle(track["Title"].asString());
	m_tag.SetAlbum(track["Album"].asString());
	m_tag.SetTrackNumber(track["TrackNumber"].asInteger(0));
	numberOfTracks = track["NumberOfTracks"].asInteger(0);
	trackNumber = track["TrackNumber"].asInteger(0);
      }
      m_playerGUIData.m_totalTime = track["Duration"].asUnsignedInteger(0);
    }
  CVariant status = CDBusUtil::GetVariant("org.bluez",
					  destination_path.c_str(),
					  "org.bluez.MediaPlayer1",
					  "Status");
  if (status == NULL)
    {
      logDBusError();
    }
  else 
    {
      if (strcmp(status.asString().c_str(), "playing")== 0) 
	{  
	  CVariant position = CDBusUtil::GetVariant("org.bluez",
						    destination_path.c_str(),
						    "org.bluez.MediaPlayer1",
						    "Position");
	  if (position == NULL)
	    {
	      logDBusError();
	    }
	  else
	    {
	      m_playerGUIData.m_time = position.asUnsignedInteger(0);
	    }
	}
    }
  // update the playlist size if the number of tracks have changed
  if (m_numberOfTracks != numberOfTracks)
    {
      trackDetailsChanged = true;
      m_numberOfTracks = numberOfTracks;
      CFileItemList list;
      for (int count =0; count < numberOfTracks; count++)
	{
	  CFileItemPtr item;
	  item.reset(new CFileItem(destination_path, false));
	  list.Add(item);
	}
      auto fileItemList = new CFileItemList();
      fileItemList->Copy(list);
      CApplicationMessenger::GetInstance().SendMsg(TMSG_PLAYLISTPLAYER_CLEAR, 0);
      CApplicationMessenger::GetInstance().SendMsg(TMSG_PLAYLISTPLAYER_ADD,0, 0,static_cast<void*>(fileItemList));
    }
  // update progress bar
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  if (trackDetailsChanged)
  {
    CApplicationMessenger::GetInstance().PostMsg(TMSG_UPDATE_CURRENT_ITEM, 0, -1, static_cast<void*>(new CFileItem(m_tag)));
    CApplicationMessenger::GetInstance().PostMsg(TMSG_PLAYLISTPLAYER_SET_SONG_NO, trackNumber, 0, NULL); 
  }
}
#endif

void BTPlayer::Process ()
{
  m_isPlaying = true;
  if (!m_startEvent.WaitMSec (100))
  {
    CLog::Log (LOGDEBUG, "BTPlayer::Process - Failed to receive start event");
    return;
  }
  m_callback.OnPlayBackStarted ();
  while (m_isPlaying && !m_bStop)
  {
#ifdef HAS_DBUS
    if (!m_dbus_path.empty()) {
      updateA2DPInfo(m_dbus_path);
    }
#endif
    Sleep(500);
  }
  m_callback.OnPlayBackStopped ();
}


void BTPlayer::OnExit()
{

}

void BTPlayer::RegisterAudioCallback(IAudioCallback* pCallback)
{
  CSingleLock lock(m_streamsLock);
  m_audioCallback = pCallback;
}

void BTPlayer::UnRegisterAudioCallback()
{
  CSingleLock lock(m_streamsLock);
  m_audioCallback = NULL;
}

void BTPlayer::OnNothingToQueueNotify()
{
  m_isFinished = true;
}

bool BTPlayer::IsPlaying() const
{
  return m_isPlaying;
}

// the only thing we want to do here is trigger the callback so we can pick up the event from Python
void BTPlayer::Pause()
{
    if (m_isPaused)
    {
        m_isPaused = false;
        m_callback.OnPlayBackResumed();
    }
    else
    {
        m_isPaused = true;
        m_callback.OnPlayBackPaused();
    }
}

bool BTPlayer::IsPaused() const
{
  return m_isPaused;
}

int64_t BTPlayer::GetTimeInternal()
{
  return 0;
}

int64_t BTPlayer::GetTime()
{
  return m_playerGUIData.m_time;
}

int64_t BTPlayer::GetTotalTime()
{
  return m_playerGUIData.m_totalTime;
}

int BTPlayer::GetCacheLevel() const
{
  return m_playerGUIData.m_cacheLevel;
}

void BTPlayer::GetAudioStreamInfo(int index, SPlayerAudioStreamInfo &info)
{
  info.bitrate = m_playerGUIData.m_audioBitrate;
  info.channels = m_playerGUIData.m_channelCount;
  info.audioCodecName = m_playerGUIData.m_codec;
  info.samplerate = m_playerGUIData.m_sampleRate;
  info.bitspersample = m_playerGUIData.m_bitsPerSample;
}

void BTPlayer::OnJobComplete(unsigned int jobID, bool success, CJob *job)
{
  CSingleLock lock(m_streamsLock);
  m_jobCounter--;
  m_jobEvent.Set();
}

void BTPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
}

void BTPlayer::SetSpeed(float iSpeed)
{
}

float BTPlayer::GetSpeed()
{
  return 1;
}

float BTPlayer::GetPercentage()
{
  if (m_playerGUIData.m_totalTime > 0)
    return m_playerGUIData.m_time * 100.0f / m_playerGUIData.m_totalTime;

  return 0.0f;
}

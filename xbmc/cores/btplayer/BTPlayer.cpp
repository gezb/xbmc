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

#ifdef HAS_DBUS
  DBusError dbus_error;
  dbus_error_init (&dbus_error);
  m_connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &dbus_error);

  if (m_connection)
  {
    dbus_connection_set_exit_on_disconnect(m_connection, false);
    if (dbus_error_is_set(&dbus_error))
    {
      CLog::Log(LOGERROR, "Bluetooth: Failed to get system bus %s", dbus_error.message);
      dbus_connection_close(m_connection);
      dbus_connection_unref(m_connection);
      m_connection = NULL;
    }
  }
  dbus_error_free (&dbus_error);
#endif
}

BTPlayer::~BTPlayer()
{
  CloseFile();
  delete m_FileItem;
#ifdef HAS_DBUS
  if (m_connection)
  {
    dbus_connection_close(m_connection);
    dbus_connection_unref(m_connection);
    m_connection = NULL;
  }
#endif
}

bool BTPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  StopThread();
  m_isPaused = false;
  CLog::Log (LOGINFO, "MAC Address %s", file.GetPath().c_str());
  m_dbus_path = file.GetPath();
  CSharedLock lock(m_streamsLock);
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
    CExclusiveLock lock(m_streamsLock);
    m_jobCounter++;
  }
  return true;
}

bool BTPlayer::CloseFile(bool reopen)
{
  CLog::Log(LOGDEBUG, "BTPlayer::CloseFile");
  m_bStop = true;
  m_dbus_path.clear();
  StopThread(true);
  CSharedLock lock(m_streamsLock);
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
void BTPlayer::updateA2DPInfo(std::string& destination_path)
{
  // get details from bluez
    CVariant properties = CDBusUtil::GetAll("org.bluez",
                                            destination_path.c_str(),
                                            "org.freedesktop.DBus.Properties",
                                            "GetAll",
                                            "org.bluez.MediaPlayer1");

    m_tag.SetArtist(properties["Track"][0]["Artist"].asString());
    m_tag.SetTitle(properties["Track"][0]["Title"].asString());
    m_tag.SetAlbum(properties["Track"][0]["Album"].asString());
    m_tag.SetTrackNumber(properties["Track"][0]["TrackNumber"].asInteger(0));
    CApplicationMessenger::GetInstance().PostMsg(TMSG_UPDATE_CURRENT_ITEM, 1,-1, static_cast<void*>(new CFileItem(m_tag)));
    const char* status = properties["Status"].asString().c_str();
    if (strcmp(status, "playing")== 0) 
    {  
      m_playerGUIData.m_time = properties["Position"].asUnsignedInteger();
      m_playerGUIData.m_totalTime = properties["Track"][0]["Duration"].asUnsignedInteger(0);
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
  Sleep(500);
#endif
  }
  m_callback.OnPlayBackStopped ();
}


void BTPlayer::OnExit()
{

}

void BTPlayer::RegisterAudioCallback(IAudioCallback* pCallback)
{
  CSharedLock lock(m_streamsLock);
  m_audioCallback = pCallback;
}

void BTPlayer::UnRegisterAudioCallback()
{
  CSharedLock lock(m_streamsLock);
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
  CExclusiveLock lock(m_streamsLock);
  m_jobCounter--;
  m_jobEvent.Set();
}

void BTPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
}

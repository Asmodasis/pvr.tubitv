/*
 *  Copyright (C) 2026 Asmodasis (https://github.com/Asmodasis)
 *  Copyright (C) 2026 Team Kodi (https://kodi.tv)
 * 
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */
#pragma once

#include "Url.cpp"

#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/addon-instance/PVR.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

static const char* LIVE_PAGE_URL = "https://tubitv.com/live";
static const char* EPG_URL = "https://epg-cdn.production-public.tubi.io/content/epg/programming";
static const char* TUBI_TV_USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";


namespace TubiTV
{

// EPG entry - one "programs[]" item for a channel row
struct TubiEpgEntry
{
  std::string title;
  std::string description;
  time_t      startTime{0}; ///< Parsed from ISO-8601 UTC string
  time_t      endTime{0};
};

// Channel - one "rows[]" item — a Tubi live channel
struct TubiChannel
{
  std::string            contentId;   ///< e.g. "555129" — used as the stable channel key
  std::string            title;
  std::string            logoUrl;     ///< images.thumbnail[0]
  std::string            manifestUrl; ///< video_resources[0].manifest.url — opaque, signed, used as-is
  int                    channelNumber{0};
  int                    iUniqueID;
  std::vector<TubiEpgEntry>  programs;
  kodi::addon::PVRChannelGroup m_group;
  //int                    genre;

};

} // namespace TubiTV


class CTubitvData : public kodi::addon::CInstancePVRClient
{
public:
  CTubitvData() = delete;
  CTubitvData(TubiTV::TubiChannel) = delete;
  CTubitvData(TubiTV::TubiEpgEntry) = delete;
  //CTubitvData(const CTubitvData&) = delete;
  //CTubitvData(CTubitvData&&) = delete;
  //CTubitvData& operator=(const CTubitvData&) = delete;
  //CTubitvData& operator=(CTubitvData&&) = delete;
  
  CTubitvData(const kodi::addon::IInstanceInfo& instance);
  ~CTubitvData() = default;

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;

  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results) override;
  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override;

  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE source, std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override;

  bool LoadChannelData();


private:

  bool FetchLivePageData(std::string& jsonOut);
  bool ExtractContentIds(const nlohmann::json& j, std::vector<std::string>& out);
  bool FetchEpgProgramming(const std::vector<std::string>& contentIds, std::string& jsonOut);
  void ParseProgrammingResponse(const nlohmann::json& j, std::vector<TubiTV::TubiChannel>& outChannels);
  bool ParseRow(const nlohmann::json& jRow, TubiTV::TubiChannel& out);
  void ParsePrograms(const nlohmann::json& jPrograms, TubiTV::TubiChannel& ch);
  bool ParseProgramEntry(const nlohmann::json& jProgram, TubiTV::TubiEpgEntry& out);


  static time_t ParseDateTime(const std::string& isoString);
  static std::string FirstThumbnail(const nlohmann::json& jImages);
  static int MapGenreToKodi(const std::string& title, const std::string& description);
  
  int ChannelUidToIndex(int uid) const;

  mutable std::mutex m_mutex;
  std::vector<TubiTV::TubiChannel> m_channels;
  std::map<int, int> m_uidToIndex; // convert uid to index for m_channels

  std::string m_deviceId;
  std::string m_userAgent;

  time_t m_nextRefresh{0};

  static constexpr int kRefreshIntervalSec{1800}; // 30 * 60
  static constexpr size_t kMaxResponseBytes{16777216}; //16 * 1024 * 1024
  static constexpr size_t kBatchSize{150};

};

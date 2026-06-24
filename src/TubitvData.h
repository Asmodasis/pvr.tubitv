/*
 *  Copyright (C) 2024 Tubi TV PVR add-on Contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include "kodi/addon-instance/PVR.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <vector>
#include <map>

/**
 * User Agent for HTTP Requests
 */
static const std::string TUBITIVE_USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                               "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";

class ATTR_DLL_LOCAL TubitvData : public kodi::addon::CAddonBase,
                                  public kodi::addon::CInstancePVRClient
{
public:
  TubitvData() = default;
  TubitvData(const TubitvData&) = delete;
  TubitvData(TubitvData&&) = delete;
  TubitvData& operator=(const TubitvData&) = delete;
  TubitvData& operator=(TubitvData&&) = delete;

  ADDON_STATUS Create() override;
  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override;

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;

  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                   kodi::addon::PVRChannelGroupMembersResultSet& results) override;
  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel& channel,
      PVR_SOURCE source,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetEPGForChannel(int channelUid,
                             time_t start,
                             time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;

private:
  struct TubiChannel
  {
    int iUniqueId;
    std::string tubiContentId;
    std::string tubiManifestId;
    int iChannelNumber; // position
    std::string strChannelName;
    std::string strIconPath;
    std::string strStreamURL;
  };

  struct TubiProgram
  {
    std::string title;
    std::string description;
    int64_t startTime;
    int64_t endTime;
    std::string thumbnail;
    std::string genre;
  };

  std::shared_ptr<nlohmann::json> m_epg_cache_document;
  time_t m_epg_cache_start = time_t(0);
  time_t m_epg_cache_end = time_t(0);

  std::vector<TubiChannel> m_channels;
  bool m_bChannelsLoaded = false;

  std::string m_deviceId;
  std::map<std::string, std::string> m_contentIdToManifestId; // Maps content_id to manifest_id for stream lookup

  std::string GetChannelStreamURL(int uniqueId);
  int GetSettingsStartChannel() const;
  bool GetSettingsColoredChannelLogos() const;
  bool GetSettingsWorkaroundBrokenStreams() const;
  void SetStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                           const std::string& url,
                           bool realtime);
  bool LoadChannelsData();

  std::string GetEpgJson() const;
  std::string GenerateManifestURL(const std::string& manifestId);
  bool ParseEPGResponse(const std::string& jsonResponse);
};
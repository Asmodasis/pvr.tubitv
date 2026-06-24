/*
 *  Copyright (C) 2024 Tubi TV PVR add-on Contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "TubitvData.h"

#include "Curl.h"
#include "Utils.h"
#include "kodi/tools/StringUtils.h"

#include <cctype>
#include <ctime>
#include <iomanip>
#include <ios>
#include <sstream>

// Tubi Live TV API endpoints
static const std::string TUBI_EPG_ENDPOINT = "https://epg-cdn.production-public.tubi.io/content/epg/programming";
static const std::string TUBI_MANIFEST_ENDPOINT = "https://live-manifest.production-public.tubi.io/live";

ADDON_STATUS TubitvData::Create()
{
  kodi::Log(ADDON_LOG_DEBUG, "%s - Creating the Tubi TV PVR add-on", __FUNCTION__);
  
  // Generate or retrieve device ID
  m_deviceId = kodi::addon::GetSettingString("device_id");
  if (m_deviceId.empty())
  {
    m_deviceId = Utils::CreateUUID();
    kodi::Log(ADDON_LOG_DEBUG, "device_id (generated): %s", m_deviceId.c_str());
    kodi::addon::SetSettingString("device_id", m_deviceId);
  }
  
  return ADDON_STATUS_OK;
}

ADDON_STATUS TubitvData::SetSetting(const std::string& settingName,
                                    const kodi::addon::CSettingValue& settingValue)
{
  return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR TubitvData::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsTV(true);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR TubitvData::GetBackendName(std::string& name)
{
  name = "Tubi TV Live PVR add-on";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR TubitvData::GetBackendVersion(std::string& version)
{
  version = STR(IPTV_VERSION);
  return PVR_ERROR_NO_ERROR;
}

namespace
{
// http://stackoverflow.com/a/17708801
const std::string UrlEncode(const std::string& value)
{
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (auto c : value)
  {
    // Keep alphanumeric and other accepted characters intact
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      escaped << c;
      continue;
    }

    // Any other characters are percent-encoded
    escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
  }

  return escaped.str();
}
} // unnamed namespace

void TubitvData::SetStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                                     const std::string& url,
                                     bool realtime)
{
  kodi::Log(ADDON_LOG_DEBUG, "[PLAY STREAM] url: %s", url.c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, realtime ? "true" : "false");
  // HLS
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/x-mpegURL");

  const std::string encodedUserAgent{UrlEncode(TUBITIVE_USER_AGENT)};
  properties.emplace_back("inputstream.adaptive.manifest_headers",
                          "User-Agent=" + encodedUserAgent);
  properties.emplace_back("inputstream.adaptive.stream_headers", "User-Agent=" + encodedUserAgent);

  if (GetSettingsWorkaroundBrokenStreams())
    properties.emplace_back("inputstream.adaptive.manifest_config",
                            "{\"hls_ignore_endlist\":true,\"hls_fix_mediasequence\":true,\"hls_fix_"
                            "discsequence\":true}");
}

int TubitvData::GetSettingsStartChannel() const
{
  return kodi::addon::GetSettingInt("start_channelnum", 1);
}

bool TubitvData::GetSettingsColoredChannelLogos() const
{
  return kodi::addon::GetSettingBoolean("colored_channel_logos", true);
}

bool TubitvData::GetSettingsWorkaroundBrokenStreams() const
{
  return kodi::addon::GetSettingBoolean("workaround_broken_streams", true);
}

std::string TubitvData::GenerateManifestURL(const std::string& manifestId)
{
  // Generate HLS playlist URL for the given manifest ID
  // Format: https://live-manifest.production-public.tubi.io/live/{manifestId}/playlist.m3u8?token={jwt_token}
  // Note: Real JWT token would need to be obtained from authentication
  // For now, we'll return a URL template that Kodi can fetch
  std::string url = TUBI_MANIFEST_ENDPOINT + "/" + manifestId + "/playlist.m3u8";
  kodi::Log(ADDON_LOG_DEBUG, "[GenerateManifestURL] Generated URL: %s", url.c_str());
  return url;
}

bool TubitvData::ParseEPGResponse(const std::string& jsonResponse)
{
  if (jsonResponse.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "[ParseEPGResponse] Empty JSON response");
    return false;
  }

  try
  {
    nlohmann::json epgDoc = nlohmann::json::parse(jsonResponse.c_str());
    
    if (epgDoc.is_discarded())
    {
      kodi::Log(ADDON_LOG_ERROR, "[ParseEPGResponse] ERROR: error while parsing json");
      return false;
    }

    // Tubi's EPG response structure
    if (!epgDoc.contains("data"))
    {
      kodi::Log(ADDON_LOG_ERROR, "[ParseEPGResponse] No 'data' field in response");
      return false;
    }

    const auto& data = epgDoc.at("data");
    kodi::Log(ADDON_LOG_DEBUG, "[ParseEPGResponse] Found %i content items", data.size());

    // Use configured start channel number to populate the channel list
    int i = GetSettingsStartChannel();
    for (const auto& item : data)
    {
      if (!item.contains("id") || !item.contains("manifest_id"))
      {
        kodi::Log(ADDON_LOG_DEBUG, "[ParseEPGResponse] Skipping item without id or manifest_id");
        continue;
      }

      const std::string contentId = item.at("id");
      const std::string manifestId = item.at("manifest_id");

      TubiChannel channel;
      channel.iChannelNumber = i++; // position
      kodi::Log(ADDON_LOG_DEBUG, "[channel] channelnr(pos): %i;", channel.iChannelNumber);

      channel.tubiContentId = contentId;
      channel.tubiManifestId = manifestId;
      kodi::Log(ADDON_LOG_DEBUG, "[channel] Tubi Content ID: %s; Manifest ID: %s", 
                channel.tubiContentId.c_str(), channel.tubiManifestId.c_str());

      const int uniqueId = Utils::Hash(contentId);
      channel.iUniqueId = uniqueId;
      kodi::Log(ADDON_LOG_DEBUG, "[channel] id: %i;", uniqueId);

      // Store mapping for quick lookup
      m_contentIdToManifestId[contentId] = manifestId;

      // Channel name
      if (item.contains("title"))
      {
        channel.strChannelName = item.at("title");
        kodi::Log(ADDON_LOG_DEBUG, "[channel] name: %s;", channel.strChannelName.c_str());
      }
      else
      {
        channel.strChannelName = "Channel " + std::to_string(channel.iChannelNumber);
      }

      // Channel logo/thumbnail
      std::string logo;
      if (item.contains("thumbnail_url"))
      {
        logo = item.at("thumbnail_url");
        kodi::Log(ADDON_LOG_DEBUG, "[channel] thumbnail: %s;", logo.c_str());
      }

      channel.strIconPath = logo;

      // Generate stream URL - this will be used to fetch the manifest
      channel.strStreamURL = GenerateManifestURL(manifestId);
      kodi::Log(ADDON_LOG_DEBUG, "[channel] streamURL: %s;", channel.strStreamURL.c_str());

      m_channels.emplace_back(channel);
    }

    return true;
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "[ParseEPGResponse] Exception: %s", e.what());
    return false;
  }
}

bool TubitvData::LoadChannelsData()
{
  if (m_bChannelsLoaded)
    return true;

  kodi::Log(ADDON_LOG_DEBUG, "[load data] GET CHANNELS");

  const std::string jsonChannels = GetEpgJson();

  if (jsonChannels.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "[channels] ERROR - empty response");
    return false;
  }

  kodi::Log(ADDON_LOG_DEBUG, "[channels] length: %i;", jsonChannels.size());
  kodi::Log(ADDON_LOG_DEBUG, "[channels] %s;", jsonChannels.c_str());

  // parse channels
  kodi::Log(ADDON_LOG_DEBUG, "[channels] parse channels");
  if (!ParseEPGResponse(jsonChannels))
  {
    kodi::Log(ADDON_LOG_ERROR, "[LoadChannelData] ERROR: error while parsing EPG");
    return false;
  }

  m_bChannelsLoaded = true;
  return true;
}

PVR_ERROR TubitvData::GetChannelsAmount(int& amount)
{
  kodi::Log(ADDON_LOG_DEBUG, "Tubi TV function call: [%s]", __FUNCTION__);

  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return PVR_ERROR_SERVER_ERROR;

  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR TubitvData::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  kodi::Log(ADDON_LOG_DEBUG, "Tubi TV function call: [%s]", __FUNCTION__);

  if (!radio)
  {
    LoadChannelsData();
    if (!m_bChannelsLoaded)
      return PVR_ERROR_SERVER_ERROR;

    for (const auto& channel : m_channels)
    {
      kodi::addon::PVRChannel kodiChannel;

      kodiChannel.SetUniqueId(channel.iUniqueId);
      kodiChannel.SetIsRadio(false);
      kodiChannel.SetChannelNumber(channel.iChannelNumber);
      kodiChannel.SetChannelName(channel.strChannelName);
      kodiChannel.SetIconPath(channel.strIconPath);
      kodiChannel.SetIsHidden(false);

      results.Add(kodiChannel);
    }
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR TubitvData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    PVR_SOURCE source,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const std::string strUrl = GetChannelStreamURL(channel.GetUniqueId());
  kodi::Log(ADDON_LOG_DEBUG, "Stream URL -> %s", strUrl.c_str());
  PVR_ERROR ret = PVR_ERROR_FAILED;
  if (!strUrl.empty())
  {
    SetStreamProperties(properties, strUrl, true);
    ret = PVR_ERROR_NO_ERROR;
  }
  return ret;
}

std::string TubitvData::GetChannelStreamURL(int uniqueId)
{
  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return {};

  for (const auto& channel : m_channels)
  {
    if (channel.iUniqueId == uniqueId)
    {
      kodi::Log(ADDON_LOG_DEBUG, "Get live url for channel %s", channel.strChannelName.c_str());
      kodi::Log(ADDON_LOG_DEBUG, "stream URL: %s", channel.strStreamURL.c_str());
      return channel.strStreamURL;
    }
  }
  return {};
}

PVR_ERROR TubitvData::GetChannelGroupsAmount(int& amount)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR TubitvData::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR TubitvData::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                             kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR TubitvData::GetEPGForChannel(int channelUid,
                                       time_t start,
                                       time_t end,
                                       kodi::addon::PVREPGTagsResultSet& results)
{
  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return PVR_ERROR_SERVER_ERROR;

  // Find channel data
  for (const auto& channel : m_channels)
  {
    if (channel.iUniqueId != channelUid)
      continue;

    // Channel data found
    // For Tubi, EPG data is typically provided with the channel data
    // You would need to parse program information from the EPG response
    // This is a simplified implementation that would need to be enhanced
    // with actual program scheduling data from Tubi's API

    kodi::Log(ADDON_LOG_DEBUG, "[epg-tubi] EPG data requested for channel: %s", 
              channel.strChannelName.c_str());
    
    // Note: Tubi's programming information would come from the initial EPG response
    // and would need to be stored and parsed for individual programs/timeslots
    // This is where you'd add program details like:
    // - Program title
    // - Description
    // - Duration
    // - Genre
    // - Thumbnail
    
    return PVR_ERROR_NO_ERROR;
  }

  kodi::Log(ADDON_LOG_ERROR, "[GetEPG] ERROR: channel not found");
  return PVR_ERROR_INVALID_PARAMETERS;
}

std::string TubitvData::GetEpgJson() const
{
  // Build the EPG request URL with required parameters
  // You can expand this list of content IDs as needed
  // These are the content IDs from your example
  std::string contentIds = "555129,578086,560215,400000106,400000286,400000285,"
                           "400000046,400000055,400000157,557344,400000045";

  std::string url = TUBI_EPG_ENDPOINT;
  url += "?platform=web";
  url += "&device_id=" + m_deviceId;
  url += "&limit_resolutions%5B%5D=h264_1080p";
  url += "&limit_resolutions%5B%5D=h265_1080p";
  url += "&lookahead=1";
  url += "&content_id=" + contentIds;

  Curl curl;
  curl.AddHeader("User-Agent", TUBITIVE_USER_AGENT);
  curl.AddHeader("Accept", "application/json");
  curl.AddHeader("Accept-Language", "en-US,en;q=0.9");

  int statusCode{500};
  const std::string json{curl.Get(url, statusCode)};

  if (statusCode == 200)
  {
    kodi::Log(ADDON_LOG_DEBUG, "[GetEpgJson] Response: %s.", json.c_str());
    return json;
  }

  kodi::Log(ADDON_LOG_ERROR, "[GetEpgJson] ERROR. status: %i, body: %s", statusCode, json.c_str());
  return "";
}

ADDONCREATOR(TubitvData)
/*
 *  Copyright (C) 2026 Asmodasis (https://github.com/Asmodasis)
 *  Copyright (C) 2026 Team Kodi (https://kodi.tv)
 * 
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */
#include "TubitvData.h"


CTubitvData::CTubitvData(const kodi::addon::IInstanceInfo& instance): CInstancePVRClient(instance)
{

  m_userAgent = kodi::addon::GetSettingString("user_agent", TUBI_TV_USER_AGENT);
  m_deviceId  = kodi::addon::GetSettingString("device_id", "");

  if (m_deviceId.empty())
  {
    static thread_local std::mt19937_64 rng(static_cast<unsigned long long>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> hexDigit(0, 15);
    const char* hex = "0123456789abcdef";
    std::ostringstream uuid;
    const char uuidLayout[] = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char c : uuidLayout)
    {
      if (c == 'x')
        uuid << hex[hexDigit(rng)];
      else if (c == 'y')
        uuid << hex[(hexDigit(rng) & 0x3) | 0x8];
      else if (c == '4' || c == '-')
        uuid << c;
      else if (c == '\0')
        break;
    }
    m_deviceId = uuid.str();
    kodi::addon::SetSettingString("device_id", m_deviceId);
  }
  
  if (!LoadChannelData())
    kodi::Log(ADDON_LOG_ERROR,"{CTubitvData}: Initial channel load failed. Check network connectivity to tubitv.com");
}

PVR_ERROR CTubitvData::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsChannelGroups(false);
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsRecordings(false);
  capabilities.SetSupportsTimers(false);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetBackendName(std::string& name)
{
  name = "Tubi TV Live";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetBackendVersion(std::string& version)
{
  version = "1.0.0";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetChannelsAmount(int& amount)
{

  //std::lock_guard<std::mutex> lock(m_mutex);
  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results)
{
  
  if (bRadio)
    return PVR_ERROR_NO_ERROR;

  if (std::time(nullptr) >= m_nextRefresh)
    LoadChannelData();

  std::lock_guard<std::mutex> lock(m_mutex);

  for (int i = 0; i < static_cast<int>(m_channels.size()); ++i)
  {
    const auto& ch = m_channels[i];
    kodi::addon::PVRChannel kodiCh;
    kodiCh.SetUniqueId(i + 1);
    kodiCh.SetIsRadio(false);
    kodiCh.SetChannelNumber(ch.channelNumber > 0 ? ch.channelNumber : i + 1);
    kodiCh.SetChannelName(ch.title);
    kodiCh.SetIconPath(ch.logoUrl);
    results.Add(kodiCh);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(channelUid);
  if (idx < 0)
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: GetEPG — unknown channel uid %d", channelUid);
    return PVR_ERROR_INVALID_PARAMETERS;
  }


  int broadcastUid = channelUid * 100000;
  for (const auto& entry : m_channels[idx].programs)
  {
    if (entry.endTime <= start || entry.startTime >= end)
      continue;

    kodi::addon::PVREPGTag tag;
    tag.SetUniqueBroadcastId(broadcastUid++);
    tag.SetUniqueChannelId(channelUid);
    tag.SetTitle(entry.title);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    tag.SetGenreType(MapGenreToKodi(entry.title, entry.description));
    results.Add(tag);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, PVR_SOURCE source, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(channel.GetUniqueId());
  if (idx < 0)
    return PVR_ERROR_INVALID_PARAMETERS;

  const auto& ch = m_channels[idx];

  if (ch.manifestUrl.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: No manifest URL cached for channel '%s' (content_id %s).", ch.title.c_str(), ch.contentId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, ch.manifestUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE,  "application/vnd.apple.mpegurl");
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back("inputstream.adaptive.manifest_type", "hls");

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CTubitvData::GetChannelGroupsAmount(int& amount)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR CTubitvData::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR CTubitvData::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                              kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}


bool CTubitvData::LoadChannelData()
{
  // discover content_ids from the live page
  std::string pageJsonText;
  if (!FetchLivePageData(pageJsonText))
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: Could not extract window.__data from tubitv.com/live. ");
    return false;
  }

  nlohmann::json pageJson;
  try
  {
    pageJson = nlohmann::json::parse(pageJsonText);
  }
  catch (const nlohmann::json::parse_error& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: window.__data JSON parse_error at byte %zu — %s", e.byte, e.what());
    return false;
  }

  std::vector<std::string> contentIds;
  if (!ExtractContentIds(pageJson, contentIds))
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: No content_ids found in window.__data.epg.contentIdsByContainer.");
    return false;
  }

  kodi::Log(ADDON_LOG_INFO, "{CTubitvData}: Discovered %zu channel content_ids.", contentIds.size());

  // fetch EPG and manifest
  std::vector<TubiTV::TubiChannel> parsed;
  parsed.reserve(contentIds.size());

  for (size_t offset = 0; offset < contentIds.size(); offset += kBatchSize)
  {
    const size_t batchEnd = std::min(offset + kBatchSize, contentIds.size());
    std::vector<std::string> batch(contentIds.begin() + offset, contentIds.begin() + batchEnd);

    std::string rawJson;
    if (!FetchEpgProgramming(batch, rawJson))
    {
      kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: EPG batch fetch failed for offset %zu (%zu ids) — skipping.", offset, batch.size());
      continue;
    }

    nlohmann::json j;
    try
    {
      j = nlohmann::json::parse(rawJson);
    }
    catch (const nlohmann::json::parse_error& e)
    {
      kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: EPG batch JSON parse_error at byte %zu — %s", e.byte, e.what());
      continue;
    }

    ParseProgrammingResponse(j, parsed);
  }

  if (parsed.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,"{CTubitvData}: 0 channels parsed from %zu discovered content_ids.", contentIds.size());
    return false;
  }

  // Assign sequential channel numbers and build the uid map.
  std::map<int, int> uidMap;
  for (int i = 0; i < static_cast<int>(parsed.size()); ++i)
  {
    parsed[i].channelNumber = i + 1;
    uidMap[i + 1] = i;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_channels   = std::move(parsed);
    m_uidToIndex = std::move(uidMap);
  } // mutex scope

  m_nextRefresh = std::time(nullptr) + kRefreshIntervalSec;
  kodi::Log(ADDON_LOG_INFO, "{CTubitvData}: Loaded %zu channels.", m_channels.size());
  return true;
}

bool CTubitvData::FetchLivePageData(std::string& jsonOut)
{
  std::string url = LIVE_PAGE_URL;
  url += BuildCurlHeaderOptions(m_userAgent);

  kodi::vfs::CFile file;
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: Cannot open %s", LIVE_PAGE_URL);
    return false;
  }

  std::string html;
  html.reserve(524288);
  char buf[8192];
  ssize_t n;
  while ((n = file.Read(buf, sizeof(buf))) > 0)
  {
    html.append(buf, static_cast<size_t>(n));
    if (html.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: livetv page too large.");
      file.Close();
      return false;
    }
  }
  file.Close();


  if (html.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: Empty response from %s", LIVE_PAGE_URL);
    return false;
  }

  // Find the <script> tag whose content starts with "window.__data" 

  std::string scriptContent{};
  std::string::size_type scriptPosStart = html.find("<script>window.__data");  

  if(html.find("<script>window.__data") == std::string::npos)
  {
    kodi::Log(ADDON_LOG_ERROR,"{CTubitvData}: Could not find a <script> tag starting with 'window.__data' in the livetv page HTML.");
    return false;
  }
  
  std::string::size_type scriptPosEnd = html.find("</script>", scriptPosStart);  

  for (int i = scriptPosStart; i < scriptPosEnd; ++i)
  {
    scriptContent.push_back(html.at(i));
  }
  // add the script on the end for the html page
  scriptContent.append("</script>");
  
  const size_t startIdx = scriptContent.find('{');
  const size_t endIdx   = scriptContent.rfind('}');
  if (startIdx == std::string::npos || endIdx == std::string::npos || endIdx < startIdx)
  {
    kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: Could not isolate JSON object braces in window.__data script.");
    return false;
  }

  std::string jsonCandidate = scriptContent.substr(startIdx, endIdx - startIdx + 1);

  // replace undefined with null
  jsonCandidate = std::regex_replace(jsonCandidate, std::regex(R"(\bundefined\b)"), "null");
  // replace the date constructor with just the date by backreference
  jsonCandidate = std::regex_replace(jsonCandidate, std::regex(R"(new Date\(\"([^\"]*)\"\))"), "\"$1\"");
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    jsonOut = std::move(jsonCandidate);
  } // mutex scope
  return true;
}

bool CTubitvData::ExtractContentIds(const nlohmann::json& j, std::vector<std::string>& out)
{

  auto contentIdStep = [&out](const nlohmann::json& item)
  {
    auto itEpg = item.find("epg");
    if (itEpg == item.end() || !itEpg->is_object())
      return;

    auto itContainers = itEpg->find("contentIdsByContainer");
    if (itContainers == itEpg->end() || !itContainers->is_object())
      return;

    for (const auto& [containerKey, containerList] : itContainers->items())
    {
      if (!containerList.is_array())
        continue;

      for (const auto& category : containerList)
      {
        auto itContents = category.find("contents");
        if (itContents == category.end() || !itContents->is_array())
          continue;

        for (const auto& idVal : *itContents)
        {
          if (idVal.is_string())
            out.push_back(idVal.get<std::string>());
          else if (idVal.is_number_integer())
            out.push_back(std::to_string(idVal.get<long long>()));
        }
      }
    }
  };

  if (j.is_array())
  {
    for (const auto& item : j)
      contentIdStep(item);
  }
  else if (j.is_object())
  {
    contentIdStep(j);
  }

  // Deduplicate while preserving first-seen order.
  std::vector<std::string> deduped;
  deduped.reserve(out.size());
  std::map<std::string, bool> seen;
  for (auto& id : out)
  {
    if (!seen.count(id))
    {
      seen[id] = true;
      deduped.push_back(id);
    }
  }
  out = std::move(deduped);

  return !out.empty();
}

bool CTubitvData::FetchEpgProgramming(const std::vector<std::string>& contentIds, std::string& jsonOut)
{
  std::string joinedIds;
  for (size_t i = 0; i < contentIds.size(); ++i)
  {
    if (i > 0)
      joinedIds += ",";
    joinedIds += contentIds[i];
  }

  std::string url = EPG_URL;

  url += "?platform=web";
  url += "&device_id=";      
  url += UrlEncode(m_deviceId);
  url += "&limit_resolutions%5B%5D=h264_1080p";
  url += "&limit_resolutions%5B%5D=h265_1080p";
  url += "&lookahead=1";
  url += "&content_id="; 
  url += UrlEncode(joinedIds);
  url += BuildCurlHeaderOptions(m_userAgent);

  kodi::vfs::CFile file;
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTubitvData: Cannot open epg/programming URL for %zu content_ids.", contentIds.size());
    return false;
  }

  jsonOut.clear();
  jsonOut.reserve(256 * 1024);
  char buf[8192];
  ssize_t n;
  while ((n = file.Read(buf, sizeof(buf))) > 0)
  {
    jsonOut.append(buf, static_cast<size_t>(n));
    if (jsonOut.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "{CTubitvData}: epg/programming response too large.");
      file.Close();
      return false;
    }
  }
  file.Close();

  return !jsonOut.empty();
}

void CTubitvData::ParseProgrammingResponse(const nlohmann::json& j, std::vector<TubiTV::TubiChannel>& outChannels)
{
  const auto itRows = j.find("rows");
  if (itRows == j.end() || !itRows->is_array())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: epg response missing 'rows' array.");
    return;
  }

  for (const auto& jRow : *itRows)
  {
    TubiTV::TubiChannel ch;
    if (ParseRow(jRow, ch))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        outChannels.push_back(std::move(ch));
    
    }
    
  }
}

bool CTubitvData::ParseRow(const nlohmann::json& jRow, TubiTV::TubiChannel& out)
{
  // content_id - required
  const auto itId = jRow.find("content_id");
  if (itId == jRow.end())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Row missing 'content_id' — skipping.");
    return false;
  }
  if (itId->is_string())
    out.contentId = itId->get<std::string>();
  else if (itId->is_number_integer())
    out.contentId = std::to_string(itId->get<long long>());
  else
    return false;

  // title - required 
  const auto itTitle = jRow.find("title");
  if (itTitle == jRow.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Row '%s' missing 'title' — skipping.", out.contentId.c_str());
    return false;
  }
  out.title = itTitle->get<std::string>();

  // images.thumbnail[0] - optional 
  const auto itImages = jRow.find("images");
  if (itImages != jRow.end() && itImages->is_object())
    out.logoUrl = FirstThumbnail(*itImages);

  // video_resources[0].manifest.url - optional but expected 
  const auto itVideoRes = jRow.find("video_resources");
  if (itVideoRes != jRow.end() && itVideoRes->is_array() && !itVideoRes->empty())
  {
    const auto& firstRes = (*itVideoRes)[0];
    const auto itManifest = firstRes.find("manifest");
    if (itManifest != firstRes.end() && itManifest->is_object())
    {
      const auto itUrl = itManifest->find("url");
      if (itUrl != itManifest->end() && itUrl->is_string())
        out.manifestUrl = itUrl->get<std::string>();
    }
  }

  if (out.manifestUrl.empty())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Channel '%s' (content_id %s) has no manifest URL", out.title.c_str(), out.contentId.c_str());
  }

  const auto itPrograms = jRow.find("programs");
  if (itPrograms != jRow.end() && itPrograms->is_array())
  {
    ParsePrograms(*itPrograms, out);

  }

  return true;
}

void CTubitvData::ParsePrograms(const nlohmann::json& jPrograms, TubiTV::TubiChannel& ch)
{
  ch.programs.reserve(jPrograms.size());
  for (const auto& jProgram : jPrograms)
  {
    TubiTV::TubiEpgEntry entry;
    if (ParseProgramEntry(jProgram, entry))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ch.programs.push_back(std::move(entry));

    }
      
  }
}

bool CTubitvData::ParseProgramEntry(const nlohmann::json& jProgram, TubiTV::TubiEpgEntry& out)
{
  const auto itTitle = jProgram.find("title");
  if (itTitle == jProgram.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Program entry missing 'title' — skipping.");
    return false;
  }

  out.title = itTitle->get<std::string>();

  const auto itStart = jProgram.find("start_time");
  const auto itEnd = jProgram.find("end_time");
  if (itStart == jProgram.end() || !itStart->is_string() ||
      itEnd   == jProgram.end() || !itEnd->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Program '%s' missing start_time/end_time — skipping.", out.title.c_str());
    return false;
  }

  out.startTime = ParseDateTime(itStart->get<std::string>());
  out.endTime   = ParseDateTime(itEnd->get<std::string>());

  if (out.startTime == 0 || out.endTime == 0 || out.endTime <= out.startTime)
  {
    kodi::Log(ADDON_LOG_WARNING, "{CTubitvData}: Program '%s' has invalid time range — skipping.", out.title.c_str());
    return false;
  }

  const auto it = jProgram.find("description"); 
  if (it != jProgram.end() && it->is_string())
  {
    out.description = it->get<std::string>();
  }

  return true;
}


time_t CTubitvData::ParseDateTime(const std::string& isoString)
{
  if (isoString.empty())
    return 0;

  std::tm tm{};
  std::istringstream ss(isoString);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

  if (ss.fail())
  {
    kodi::Log(ADDON_LOG_WARNING,"{CTubitvData}: Failed to parse time '%s'", isoString.c_str());
    return 0;
  }

#if defined(_WIN32) || defined(_WIN64)
  return static_cast<time_t>(_mkgmtime(&tm));
#else // not windows
  return static_cast<time_t>(timegm(&tm));
#endif
}

std::string CTubitvData::FirstThumbnail(const nlohmann::json& jImages)
{
  const auto it = jImages.find("thumbnail");
  if (it == jImages.end() || !it->is_array() || it->empty())
    return "";

  const auto& first = (*it)[0];
  if(first.is_string())
    return first.get<std::string>();
  else
    return "";
}

int CTubitvData::MapGenreToKodi(const std::string& title, const std::string& description)
{
  // Tubi's epg response does not include a dedicated genre
  // field on entries, map searches for keywords and assigns
  // a genre for grouping color to the guide 
  struct { const char* key; int type; } kMap[] = {
    { "News",     EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS                },
    { "news",     EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS                },
    { "Sport",    EPG_EVENT_CONTENTMASK_SPORTS                            },
    { "sport",    EPG_EVENT_CONTENTMASK_SPORTS                            },
    { "Kids",     EPG_EVENT_CONTENTMASK_CHILDRENYOUTH                     },
    { "kids",     EPG_EVENT_CONTENTMASK_CHILDRENYOUTH                     },
    { "Movie",    EPG_EVENT_CONTENTMASK_MOVIEDRAMA                        },
    { "movie",    EPG_EVENT_CONTENTMASK_MOVIEDRAMA                        },
    { "Comedy",   EPG_EVENT_CONTENTMASK_SHOW                              },
    { "comedy",   EPG_EVENT_CONTENTMASK_SHOW                              },
    { "Science",  EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE                },
    { "science",  EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE                },
    { "Culture",  EPG_EVENT_CONTENTMASK_ARTSCULTURE                       },
    { "culture",  EPG_EVENT_CONTENTMASK_ARTSCULTURE                       },
    { "Political",EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS          },
    { "political",EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS          },
  };
  for (const auto& e : kMap)
  {
    if (title.find(e.key) != std::string::npos || description.find(e.key) != std::string::npos)
      return e.type;
  }
  return EPG_EVENT_CONTENTMASK_UNDEFINED;
}

int CTubitvData::ChannelUidToIndex(int uid) const
{
  auto it = m_uidToIndex.find(uid);
  if(it != m_uidToIndex.end())
    return it->second;
  else
    return -1;
}
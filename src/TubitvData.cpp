/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  TubitvData.cpp
 *  ──────────────
 *  Every field name and response shape used below is confirmed from TWO
 *  independent real sources: the user's own captured browser request URLs,
 *  and the working Python source of github.com/BuddyChewChew/tubi-scraper.
 *  See the header comment in TubitvData.h for the full breakdown of what's
 *  confirmed from which source.
 */

#include "TubitvData.h"

#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>



// ─── Constants ────────────────────────────────────────────────────────────────

namespace
{
  static const char* LIVE_PAGE_URL = "https://tubitv.com/live";

  // Confirmed from the user's captured request. The reference scraper uses
  // the equivalent https://tubitv.com/oz/epg/programming with fewer params;
  // we use the CDN-fronted host since that's what was directly observed in
  // real browser traffic.
  static const char* EPG_URL =
      "https://epg-cdn.production-public.tubi.io/content/epg/programming";

  static const char* TUBI_TV_USER_AGENT =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/125.0.0.0 Safari/537.36";

  std::string UrlEncode(const std::string& value)
  {
    std::ostringstream out;
    out.fill('0');
    out << std::hex;
    for (unsigned char c : value)
    {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out << c;
      else
        out << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
    }
    return out.str();
  }

  /// Kodi's CURL-backed VFS reads custom request headers from pipe-delimited
  /// URL options appended after the real query string, e.g.:
  ///   https://host/path?real=query|User-Agent=Foo
  std::string BuildCurlHeaderOptions(const std::string& userAgent)
  {
    if (userAgent.empty())
      return "";
    return "|User-Agent=" + UrlEncode(userAgent);
  }
} // namespace

// ─── Constructor / Destructor ─────────────────────────────────────────────────

TubitvData::TubitvData()
{
  // Intentionally empty. See TubitvData::Init() — settings access (which
  // requires the owning addon instance to be fully attached to Kodi) must
  // not happen here, since this object is constructed from
  // CClientInstance's member-initializer list, before CClientInstance's
  // own constructor body has run.
}

bool TubitvData::Init()
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
    const char layout[] = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char c : layout)
    {
      if (c == 'x')
        uuid << hex[hexDigit(rng)];
      else if (c == 'y')
        uuid << hex[(hexDigit(rng) & 0x3) | 0x8];
      else if (c == '4')
        uuid << '4';
      else if (c == '-')
        uuid << '-';
      else if (c == '\0')
        break;
    }
    m_deviceId = uuid.str();
    kodi::addon::SetSettingString("device_id", m_deviceId);
  }

  return true;
}

TubitvData::~TubitvData() = default;

// ─── Public: LoadChannelData ──────────────────────────────────────────────────

bool TubitvData::LoadChannelData()
{
  // ── Step 1: discover content_ids from the live page ──────────────────────
  std::string pageJsonText;
  if (!FetchLivePageData(pageJsonText))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: Could not extract window.__data from tubitv.com/live. "
              "Tubi may have changed their page bundling — see ExtractContentIds "
              "comment in TubitvData.h.");
    return false;
  }

  nlohmann::json pageJson;
  try
  {
    pageJson = nlohmann::json::parse(pageJsonText);
  }
  catch (const nlohmann::json::parse_error& e)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: window.__data JSON parse_error at byte %zu — %s",
              e.byte, e.what());
    return false;
  }

  std::vector<std::string> contentIds;
  if (!ExtractContentIds(pageJson, contentIds))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: No content_ids found in window.__data.epg.contentIdsByContainer.");
    return false;
  }

  kodi::Log(ADDON_LOG_INFO,
            "TubitvData: Discovered %zu channel content_ids.", contentIds.size());

  // ── Step 2: batch-fetch EPG + manifest data ───────────────────────────────
  std::vector<TubiTV::Channel> parsed;
  parsed.reserve(contentIds.size());

  for (size_t offset = 0; offset < contentIds.size(); offset += kBatchSize)
  {
    const size_t batchEnd = std::min(offset + kBatchSize, contentIds.size());
    std::vector<std::string> batch(contentIds.begin() + offset, contentIds.begin() + batchEnd);

    std::string rawJson;
    if (!FetchEpgProgramming(batch, rawJson))
    {
      kodi::Log(ADDON_LOG_WARNING,
                "TubitvData: EPG batch fetch failed for offset %zu (%zu ids) — skipping.",
                offset, batch.size());
      continue;
    }

    nlohmann::json j;
    try
    {
      j = nlohmann::json::parse(rawJson);
    }
    catch (const nlohmann::json::parse_error& e)
    {
      kodi::Log(ADDON_LOG_ERROR,
                "TubitvData: EPG batch JSON parse_error at byte %zu — %s",
                e.byte, e.what());
      continue;
    }

    ParseProgrammingResponse(j, parsed);
  }

  if (parsed.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: 0 channels parsed from %zu discovered content_ids.",
              contentIds.size());
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
  }

  m_nextRefresh = std::time(nullptr) + kRefreshIntervalSec;
  kodi::Log(ADDON_LOG_INFO, "TubitvData: Loaded %zu channels.", m_channels.size());
  return true;
}

// ─── Public: Kodi PVR interface ───────────────────────────────────────────────

int TubitvData::GetChannelCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_channels.size());
}

PVR_ERROR TubitvData::GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results)
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

PVR_ERROR TubitvData::GetEPGForChannel(int uid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(uid);
  if (idx < 0)
  {
    kodi::Log(ADDON_LOG_WARNING, "[GetEPGForChannel]: GetEPG — unknown channel uid %d", uid);
    return PVR_ERROR_INVALID_PARAMETERS;
  }

  int broadcastUid = uid * 100000;
  for (const auto& entry : m_channels.at(idx))
  {
    if (entry.endTime <= start || entry.startTime >= end)
      continue;

    kodi::addon::PVREPGTag tag;
    tag.SetUniqueBroadcastId(broadcastUid++);
    tag.SetUniqueChannelId(uid);
    tag.SetTitle(entry.title);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    tag.SetGenreType(MapGenreToKodi(entry.title, entry.description));
    results.Add(tag);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR TubitvData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    std::vector<kodi::addon::PVRStreamProperty>& props)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(channel.GetUniqueId());
  if (idx < 0)
    return PVR_ERROR_INVALID_PARAMETERS;

  const auto& ch = m_channels[idx];

  if (ch.manifestUrl.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: No manifest URL cached for channel '%s' (content_id %s).",
              ch.title.c_str(), ch.contentId.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  props.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, ch.manifestUrl);
  props.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE,  "application/vnd.apple.mpegurl");
  props.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  props.emplace_back("inputstream.adaptive.manifest_type", "hls");

  return PVR_ERROR_NO_ERROR;
}

// ─── Private: Step 1 — channel discovery ─────────────────────────────────────

bool TubitvData::FetchLivePageData(std::string& jsonOut)
{
  std::string url = LIVE_PAGE_URL;
  url += BuildCurlHeaderOptions(m_userAgent);

  kodi::vfs::CFile file;
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR, "TubitvData: Cannot open %s", LIVE_PAGE_URL);
    return false;
  }

  std::string html;
  html.reserve(512 * 1024);
  char buf[8192];
  ssize_t n;
  while ((n = file.Read(buf, sizeof(buf))) > 0)
  {
    html.append(buf, static_cast<size_t>(n));
    if (html.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "TubitvData: livetv page too large.");
      file.Close();
      return false;
    }
  }
  file.Close();

  kodi::Log(ADDON_LOG_DEBUG, "[FetchLivePageData] html: %s", html.c_str());

  if (html.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "TubitvData: Empty response from %s", LIVE_PAGE_URL);
    return false;
  }

  // Find the <script> tag whose content starts with "window.__data" 

  std::string scriptContent{};
  std::string::size_type scriptPosStart = html.find("<script>window.__data");  

  if(html.find("<script>window.__data") == std::string::npos)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "TubitvData: Could not find a <script> tag starting with "
              "'window.__data' in the livetv page HTML.");
    return false;
  }
  
  std::string::size_type scriptPosEnd = html.find("</script>", scriptPosStart);  

  for(int i = scriptPosStart; i < scriptPosEnd; ++i)
  {
    scriptContent.push_back(html.at(i));
  }
  // add the script on the end for the html page
  scriptContent.append("</script>");
  
  
  // Isolate the JSON object literal: from the first '{' to the matching
  // last '}'. Mirrors the reference scraper's approach
  // (target_script.find("{") ... target_script.rfind("}") + 1) rather than
  // attempting a full JS-expression parse.
  
  const size_t startIdx = scriptContent.find('{');
  const size_t endIdx   = scriptContent.rfind('}');
  if (startIdx == std::string::npos || endIdx == std::string::npos || endIdx < startIdx)
  {
    kodi::Log(ADDON_LOG_ERROR, "TubitvData: Could not isolate JSON object braces in window.__data script.");
    return false;
  }

  std::string jsonCandidate = scriptContent.substr(startIdx, endIdx - startIdx + 1);

  // The reference scraper also normalizes a couple of JS-isms that are not
  // valid JSON before parsing: bare `undefined` -> `null`, and
  // `new Date("...")` wrapper calls -> just the quoted string.
  jsonCandidate = std::regex_replace(jsonCandidate, std::regex(R"(\bundefined\b)"), "null");
  jsonCandidate = std::regex_replace(
      jsonCandidate, std::regex(R"(new Date\(\"([^\"]*)\"\))"), "\"$1\"");

  jsonOut = std::move(jsonCandidate);
  return true;
}

bool TubitvData::ExtractContentIds(const nlohmann::json& j, std::vector<std::string>& out)
{
  // Confirmed shape from the reference scraper: window.__data may be either
  // a single object or (per the scraper's defensive isinstance(list) check)
  // a list of such objects. Walk: [item.]epg.contentIdsByContainer[*][*].contents[]
  auto walkOne = [&out](const nlohmann::json& item)
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
      walkOne(item);
  }
  else if (j.is_object())
  {
    walkOne(j);
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

// ─── Private: Step 2 — EPG + manifest batch fetch ────────────────────────────

bool TubitvData::FetchEpgProgramming(const std::vector<std::string>& contentIds, std::string& jsonOut)
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
  url += "&device_id=";              url += UrlEncode(m_deviceId);
  url += "&limit_resolutions%5B%5D=h264_1080p";
  url += "&limit_resolutions%5B%5D=h265_1080p";
  url += "&lookahead=1";
  url += "&content_id=";             url += UrlEncode(joinedIds);
  url += BuildCurlHeaderOptions(m_userAgent);

  kodi::vfs::CFile file;
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR, "TubitvData: Cannot open epg/programming URL for %zu content_ids.", contentIds.size());
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
      kodi::Log(ADDON_LOG_ERROR, "TubitvData: epg/programming response too large.");
      file.Close();
      return false;
    }
  }
  file.Close();

  return !jsonOut.empty();
}

void TubitvData::ParseProgrammingResponse(const nlohmann::json& j, std::vector<TubiTV::Channel>& outChannels)
{
  // Confirmed envelope shape: {"rows": [...]}. The reference scraper reads
  // epg_json.get('rows', []) — NOT a bare top-level array.
  auto itRows = j.find("rows");
  if (itRows == j.end() || !itRows->is_array())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "TubitvData: epg/programming response missing 'rows' array.");
    return;
  }

  for (const auto& jRow : *itRows)
  {
    TubiTV::Channel ch;
    if (ParseRow(jRow, ch))
      outChannels.push_back(std::move(ch));
  }
}

bool TubitvData::ParseRow(const nlohmann::json& jRow, TubiTV::Channel& out)
{
  // ── content_id — required ─────────────────────────────────────────────────
  auto itId = jRow.find("content_id");
  if (itId == jRow.end())
  {
    kodi::Log(ADDON_LOG_WARNING, "TubitvData: Row missing 'content_id' — skipping.");
    return false;
  }
  if (itId->is_string())
    out.contentId = itId->get<std::string>();
  else if (itId->is_number_integer())
    out.contentId = std::to_string(itId->get<long long>());
  else
    return false;

  // ── title — required ──────────────────────────────────────────────────────
  auto itTitle = jRow.find("title");
  if (itTitle == jRow.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "TubitvData: Row '%s' missing 'title' — skipping.", out.contentId.c_str());
    return false;
  }
  out.title = itTitle->get<std::string>();

  // ── images.thumbnail[0] — optional ───────────────────────────────────────
  if (auto itImages = jRow.find("images"); itImages != jRow.end() && itImages->is_object())
    out.logoUrl = FirstThumbnail(*itImages);

  // ── video_resources[0].manifest.url — optional but expected ──────────────
  if (auto itVideoRes = jRow.find("video_resources");
      itVideoRes != jRow.end() && itVideoRes->is_array() && !itVideoRes->empty())
  {
    const auto& firstRes = (*itVideoRes)[0];
    if (auto itManifest = firstRes.find("manifest");
        itManifest != firstRes.end() && itManifest->is_object())
    {
      if (auto itUrl = itManifest->find("url"); itUrl != itManifest->end() && itUrl->is_string())
        out.manifestUrl = itUrl->get<std::string>();
    }
  }

  if (out.manifestUrl.empty())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "TubitvData: Channel '%s' (content_id %s) has no manifest URL — "
              "it will load in the channel list but will fail to play.",
              out.title.c_str(), out.contentId.c_str());
  }

  // ── programs[] — optional ─────────────────────────────────────────────────
  if (auto itPrograms = jRow.find("programs");
      itPrograms != jRow.end() && itPrograms->is_array())
  {
    ParsePrograms(*itPrograms, out);
    //out.genre = MapGenreToKodi(out.title, out.programs.at(description))
  }

//MapGenreToKodi(const std::string& title, const std::string& description)
  
 //if(!out.genre = MapGenreToKodi(out.title, out.programs.at(description)))
 //{
 //     kodi::Log(ADDON_LOG_WARNING, "[ParseRow] no genre to map");
 //}

  return true;
}

void TubitvData::ParsePrograms(const nlohmann::json& jPrograms, TubiTV::Channel& ch)
{
  //ch.programs.reserve(jPrograms.size());
  for (const auto& jProgram : jPrograms)
  {

    //if (ParseProgramEntry(jProgram, entry))
      //ch.programs.push_back(std::move(entry));

      auto itTitle = jProgram.find("title");
      if (itTitle == jProgram.end() || !itTitle->is_string())
      {
        kodi::Log(ADDON_LOG_WARNING, "[ParsePrograms]: Program entry missing 'title' — skipping.");
      }
      ch.title = itTitle->get<std::string>();

      auto itStart = jProgram.find("start_time");
      auto itEnd   = jProgram.find("end_time");
      if (itStart == jProgram.end() || !itStart->is_string() ||
          itEnd   == jProgram.end() || !itEnd->is_string())
      {
        kodi::Log(ADDON_LOG_WARNING, "[ParsePrograms]: Program '%s' missing start_time/end_time — skipping.", ch.title.c_str());
      }

      ch.startTime = ParseISO8601(itStart->get<std::string>());
      ch.endTime   = ParseISO8601(itEnd->get<std::string>());

      if (ch.startTime == 0 || ch.endTime == 0 || ch.endTime <= ch.startTime)
      {
        kodi::Log(ADDON_LOG_WARNING,"[ParsePrograms]: Program '%s' has invalid time range — skipping.", ch.title.c_str());
      }

      if (auto it = jProgram.find("description"); it != jProgram.end() && it->is_string())
        ch.description = it->get<std::string>();


  }
}

/*
bool TubitvData::ParseProgramEntry(const nlohmann::json& jProgram, TubiTV::EpgEntry& out)
{
  auto itTitle = jProgram.find("title");
  if (itTitle == jProgram.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "TubitvData: Program entry missing 'title' — skipping.");
    return false;
  }
  out.title = itTitle->get<std::string>();

  auto itStart = jProgram.find("start_time");
  auto itEnd   = jProgram.find("end_time");
  if (itStart == jProgram.end() || !itStart->is_string() ||
      itEnd   == jProgram.end() || !itEnd->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "TubitvData: Program '%s' missing start_time/end_time — skipping.",
              out.title.c_str());
    return false;
  }

  out.startTime = ParseISO8601(itStart->get<std::string>());
  out.endTime   = ParseISO8601(itEnd->get<std::string>());

  if (out.startTime == 0 || out.endTime == 0 || out.endTime <= out.startTime)
  {
    kodi::Log(ADDON_LOG_WARNING,
              "TubitvData: Program '%s' has invalid time range — skipping.",
              out.title.c_str());
    return false;
  }

  if (auto it = jProgram.find("description"); it != jProgram.end() && it->is_string())
    out.description = it->get<std::string>();

  return true;
}
*/
// ─── Private: Utility ────────────────────────────────────────────────────────

time_t TubitvData::ParseISO8601(const std::string& isoString)
{
  if (isoString.empty())
    return 0;

  // Confirmed format "%Y-%m-%dT%H:%M:%SZ" from the reference scraper's own
  // datetime.strptime() call against this exact field.
  std::tm tm{};
  std::istringstream ss(isoString);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

  if (ss.fail())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "TubitvData: Failed to parse ISO-8601 time '%s'", isoString.c_str());
    return 0;
  }

#if defined(_WIN32) || defined(_WIN64)
  return static_cast<time_t>(_mkgmtime(&tm));
#else
  return static_cast<time_t>(timegm(&tm));
#endif
}

std::string TubitvData::FirstThumbnail(const nlohmann::json& jImages)
{
  auto it = jImages.find("thumbnail");
  if (it == jImages.end() || !it->is_array() || it->empty())
    return "";

  const auto& first = (*it)[0];
  return first.is_string() ? first.get<std::string>() : "";
}

int TubitvData::MapGenreToKodi(const std::string& title, const std::string& description)
{
  // Tubi's epg/programming response does not include a dedicated genre
  // field on program entries, key map searches for keywords and assigns
  // a genre for grouping
  struct { const char* key; int type; } kMap[] = {
    { "News",     EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS  },
    { "Sport",    EPG_EVENT_CONTENTMASK_SPORTS              },
    { "Kids",     EPG_EVENT_CONTENTMASK_CHILDRENYOUTH       },
    { "Movie",    EPG_EVENT_CONTENTMASK_MOVIEDRAMA          },
    { "Comedy",   EPG_EVENT_CONTENTMASK_SHOW                },
    { "Science",  EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE  },
  };
  for (const auto& e : kMap)
  {
    if (title.find(e.key) != std::string::npos || description.find(e.key) != std::string::npos)
      return e.type;
  }
  return EPG_EVENT_CONTENTMASK_UNDEFINED;
}

int TubitvData::ChannelUidToIndex(int uid) const
{
  auto it = m_uidToIndex.find(uid);
  return (it != m_uidToIndex.end()) ? it->second : -1;
}

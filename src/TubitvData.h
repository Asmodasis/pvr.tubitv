/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  TubitvData.h
 *  ────────────
 *  Data-layer class for the Tubi TV Live PVR addon. Mirrors the role of
 *  PlutotvData in pvr.plutotv: owns the channel cache, talks to the backend,
 *  and hands populated Kodi result-sets back to ClientInstance. Uses
 *  nlohmann/json instead of RapidJSON.
 *
 *  ─── CONFIRMED REAL API ───────────────────────────────────────────────────
 *  Unlike the Amazon Live TV investigation, this one has real, independently
 *  confirmed ground truth from TWO sources:
 *
 *    1. The user captured two real, live HTTP request URLs directly from
 *       browser traffic against tubitv.com/live (EPG fetch + manifest URL).
 *    2. A separate, real, currently-maintained public scraper project
 *       (BuddyChewChew/tubi-scraper, github.com/BuddyChewChew/tubi-scraper)
 *       contains actual working Python source code that fetches and parses
 *       this exact data, confirming the JSON field names independently of
 *       the user's captured URLs.
 *
 *  Both sources agree on the response shape below.
 *
 *  STEP 1 — Channel discovery (bulk list, confirmed via tubi-scraper source):
 *
 *    GET https://tubitv.com/live
 *
 *    The page embeds a <script> tag beginning with "window.__data = {...}"
 *    containing JSON. Walk: data.epg.contentIdsByContainer (a map of
 *    container-key -> array of { name, contents: [content_id, ...] }).
 *    Flattening every "contents" array across every container yields the
 *    full list of content_ids representing every live channel.
 *    NOTE: confirmed structurally from real source code; this addon has not
 *    independently re-fetched and parsed this exact page, so treat the
 *    *existence and shape* of window.__data as confirmed, but verify if
 *    Tubi changes their page bundling.
 *
 *  STEP 2 — EPG fetch for a batch of channels (confirmed via BOTH sources):
 *
 *    GET https://epg-cdn.production-public.tubi.io/content/epg/programming
 *        ?platform=web
 *        &device_id=<uuid>
 *        &limit_resolutions[]=h264_1080p
 *        &limit_resolutions[]=h265_1080p
 *        &lookahead=1
 *        &content_id=<comma-separated content_ids, batched>
 *
 *    (tubi-scraper's source uses the equivalent https://tubitv.com/oz/epg/programming
 *     with just `content_id`; the user's captured URL uses the CDN-fronted
 *     host with additional resolution/lookahead params. Both are plausibly
 *     the same backend behind different front doors — we use the user's
 *     captured host/params since that's what was directly observed.)
 *
 *    Response (confirmed real shape, field names from working source code):
 *    {
 *      "rows": [
 *        {
 *          "content_id": "555129",
 *          "title": "ABC News Live",
 *          "images": { "thumbnail": ["https://...png", ...] },
 *          "video_resources": [
 *            { "manifest": { "url": "https://live-manifest.production-public.tubi.io/live/<id>/playlist.m3u8?token=...&..." } }
 *          ],
 *          "programs": [
 *            {
 *              "title": "World News Tonight",
 *              "description": "...",
 *              "start_time": "2026-06-23T20:00:00Z",   // ISO-8601 UTC
 *              "end_time":   "2026-06-23T20:30:00Z"
 *            }
 *          ]
 *        }
 *      ]
 *    }
 *
 *  STEP 3 — Playback URL: NOT a separate fetch.
 *
 *    The manifest URL is already present in row.video_resources[0].manifest.url
 *    from the EPG response itself — confirmed by the user's captured manifest
 *    URL, which is a long-lived signed JWT (HS512, confirmed by decoding it:
 *    claims include device_id, country, platform, exp, and a station
 *    reference under "rss"). Because it's server-signed, this addon does
 *    NOT attempt to construct or refresh this URL itself — it is taken
 *    verbatim from the EPG response and handed to inputstream.adaptive
 *    as-is, including every query parameter (token, tb.sid, ap-fb, etc).
 *    Treat the entire query string as opaque; do not strip or rebuild it.
 * ───────────────────────────────────────────────────────────────────────────
 */

#pragma once

#include <kodi/addon-instance/PVR.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace TubiTV
{

// ─── EPG entry (one "programs[]" item for a channel row) ─────────────────────
struct EpgEntry
{
  std::string title;
  std::string description;
  time_t      startTime{0}; ///< Parsed from ISO-8601 UTC string
  time_t      endTime{0};
};

// ─── Channel (one "rows[]" item — a Tubi live channel) ───────────────────────
struct Channel
{
  std::string            contentId;   ///< e.g. "555129" — used as the stable channel key
  std::string            title;
  std::string            logoUrl;     ///< images.thumbnail[0]
  std::string            manifestUrl; ///< video_resources[0].manifest.url — opaque, signed, used as-is
  int                    channelNumber{0};
  std::vector<EpgEntry>  programs;
};

} // namespace TubiTV


class TubitvData
{
public:
  TubitvData();
  ~TubitvData();

  bool Init();

  bool LoadChannelData();

  int        GetChannelCount() const;
  PVR_ERROR  GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results);
  PVR_ERROR  GetEPGForChannel(int channelUid, time_t start, time_t end,
                              kodi::addon::PVREPGTagsResultSet& results);
  PVR_ERROR  GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                        std::vector<kodi::addon::PVRStreamProperty>& props);

private:

  bool FetchLivePageData(std::string& jsonOut);
  bool ExtractContentIds(const nlohmann::json& j, std::vector<std::string>& out);
  bool FetchEpgProgramming(const std::vector<std::string>& contentIds, std::string& jsonOut);
  void ParseProgrammingResponse(const nlohmann::json& j,
                                std::vector<TubiTV::Channel>& outChannels);
  bool ParseRow(const nlohmann::json& jRow, TubiTV::Channel& out);
  void ParsePrograms(const nlohmann::json& jPrograms, TubiTV::Channel& ch);
  bool ParseProgramEntry(const nlohmann::json& jProgram, TubiTV::EpgEntry& out);


  static time_t ParseISO8601(const std::string& isoString);
  static std::string FirstThumbnail(const nlohmann::json& jImages);
  static int MapGenreToKodi(const std::string& title, const std::string& description);
  
  int ChannelUidToIndex(int uid) const;

  // ── State ─────────────────────────────────────────────────────────────────
  mutable std::mutex             m_mutex;
  std::vector<TubiTV::Channel>   m_channels;
  std::map<int, int>             m_uidToIndex; ///< uid -> index into m_channels

  std::string m_deviceId;      ///< Persisted UUID, mirrors the device_id query param
  std::string m_userAgent;

  time_t m_nextRefresh{0};

  static constexpr int    kRefreshIntervalSec = 30 * 60;
  static constexpr size_t kMaxResponseBytes   = 16 * 1024 * 1024;
  static constexpr size_t kBatchSize          = 150; ///< Matches reference scraper's group_size
};

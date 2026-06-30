/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  ClientInstance.h
 *  ────────────────
 *  Kodi PVR addon instance class. Mirrors the pattern used in pvr.plutotv's
 *  addon.cpp / CPlutotvAddon and the earlier pvr.amazontv ClientInstance.
 *
 *  Thin delegate: owns a TubitvData object and forwards every PVR_ERROR
 *  callback through to it. Contains no Tubi-specific parsing logic itself.
 */

#pragma once

#include "TubitvData.h"

#include <kodi/addon-instance/PVR.h>

#include <memory>

class CClientInstance : public kodi::addon::CInstancePVRClient : public TubiTV
{
public:
  explicit CClientInstance(const kodi::addon::IInstanceInfo& instance);
  ~CClientInstance() override;

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& caps) override;

  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results) override;

  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;

  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel& channel,
      PVR_SOURCE source,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override;


private:
  std::unique_ptr<TubitvData> m_data;
};

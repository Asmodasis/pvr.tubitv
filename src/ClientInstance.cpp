/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  ClientInstance.cpp
 */

#include "ClientInstance.h"
#include <kodi/General.h>

CClientInstance::CClientInstance(const kodi::addon::IInstanceInfo& instance)
  : CInstancePVRClient(instance),
    m_data(std::make_unique<TubitvData>())
{
  // Init() reads/writes Kodi addon settings, which requires the addon
  // instance to be fully attached — safe here in the constructor BODY
  // (after CInstancePVRClient(instance) has completed), but NOT safe
  // inside TubitvData's own constructor, since that runs as part of this
  // class's member-initializer list, before this point. See the comment
  // on TubitvData::Init() for the full explanation.
  if (!m_data->Init())
  {
    kodi::Log(ADDON_LOG_ERROR, "CClientInstance: TubitvData::Init() failed.");
    return;
  }

  if (!m_data->LoadChannelData())
    kodi::Log(ADDON_LOG_ERROR,
              "CClientInstance: Initial channel load failed. Check network "
              "connectivity to tubitv.com and epg-cdn.production-public.tubi.io.");
}

CClientInstance::~CClientInstance() = default;

PVR_ERROR CClientInstance::GetCapabilities(kodi::addon::PVRCapabilities& caps)
{
  caps.SetSupportsTV(true);
  caps.SetSupportsRadio(false);
  caps.SetSupportsChannelGroups(false);
  caps.SetSupportsEPG(true);
  caps.SetSupportsRecordings(false);
  caps.SetSupportsTimers(false);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetBackendName(std::string& name)
{
  name = "Tubi TV Live";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetBackendVersion(std::string& version)
{
  version = "1.0.0";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetChannelsAmount(int& amount)
{
  amount = m_data->GetChannelCount();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetChannels(bool bRadio,
                                       kodi::addon::PVRChannelsResultSet& results)
{
  return m_data->GetChannels(bRadio, results);
}

PVR_ERROR CClientInstance::GetEPGForChannel(int channelUid, time_t start, time_t end,
                                            kodi::addon::PVREPGTagsResultSet& results)
{
  return m_data->GetEPGForChannel(channelUid, start, end, results);
}

PVR_ERROR CClientInstance::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    PVR_SOURCE /*source*/,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  return m_data->GetChannelStreamProperties(channel, properties);
}

PVR_ERROR CClientInstance::GetChannelGroupsAmount(int& amount)
{
  amount = m_data->getGenreCount();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  for (unsigned short i = 0; i < m_data->getGenreList().size(); ++i)
  {
    kodi::addon::PVRChannelGroup group;
    group.SetIsRadio(false);
    group.SetGroupName(m_data->getGenreList()[i]);
    group.SetPosition(i);
    // Give it now to Kodi
    results.Add(group);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                              kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  unsigned short iChannelPtr = 0;
  std::vector<TubiTV::Channel> m_chann =  m_data->getM_channels();
  for (const auto& chann : m_chann)
  {
    if (chann.programs[iChannelPtr].m_group.GetGroupName() == group.GetGroupName())
    {
     
        //TubiTV::Channel &channel = m_chann[iChannelPtr];
        kodi::addon::PVRChannelGroupMember kodiGroupMember;
        kodiGroupMember.SetGroupName(group.GetGroupName());
        kodiGroupMember.SetChannelUniqueId(std::stoi(chann.contentId));
        kodiGroupMember.SetChannelNumber(chann.channelNumber);
 
        results.Add(kodiGroupMember);

    }
    iChannelPtr++;
  }
  return PVR_ERROR_NO_ERROR;
}
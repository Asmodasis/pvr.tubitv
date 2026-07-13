/*
 *  Copyright (C) 2026 Asmodasis (https://github.com/Asmodasis)
 *  Copyright (C) 2026 Team Kodi (https://kodi.tv)
 * 
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "TubitvData.h"


class ATTR_DLL_LOCAL CTubiAddon : public kodi::addon::CAddonBase
{
public:
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    hdl = new CTubitvData(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CTubiAddon)

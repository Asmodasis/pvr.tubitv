/*
 *  Copyright (C) 2026 Asmodasis (https://github.com/Asmodasis)
 *  Copyright (C) 2026 Team Kodi (https://kodi.tv)
 * 
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */
#include <iomanip>
#include <sstream>
#include <string>

namespace
{
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

  std::string BuildCurlHeaderOptions(const std::string& userAgent)
  {
    if (userAgent.empty())
      return "";
    return "|User-Agent=" + UrlEncode(userAgent);
  }

} // unamed namespace
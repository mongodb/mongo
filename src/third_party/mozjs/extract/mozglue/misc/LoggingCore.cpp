/* vim: set shiftwidth=4 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LoggingCore.h"

#include <algorithm>

namespace mozilla {

LogLevel ToLogLevel(int32_t aLevel) {
  aLevel = std::min(aLevel, static_cast<int32_t>(LogLevel::Verbose));
  aLevel = std::max(aLevel, static_cast<int32_t>(LogLevel::Disabled));
  return static_cast<LogLevel>(aLevel);
}

}  // namespace mozilla

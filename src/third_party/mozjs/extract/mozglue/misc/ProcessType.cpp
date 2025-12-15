/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ProcessType.h"

#include <cstring>

#include "mozilla/Assertions.h"

using namespace mozilla::startup;

namespace mozilla {
namespace startup {
GeckoProcessType sChildProcessType = GeckoProcessType_Default;
GeckoChildID sGeckoChildID = 0;
}  // namespace startup

void SetGeckoProcessType(const char* aProcessTypeString) {
  if (sChildProcessType != GeckoProcessType_Default &&
      sChildProcessType != GeckoProcessType_ForkServer) {
    MOZ_CRASH("Cannot set GeckoProcessType multiple times.");
  }

#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  if (std::strcmp(aProcessTypeString, string_name) == 0) {                    \
    sChildProcessType = GeckoProcessType::GeckoProcessType_##enum_name;       \
    return;                                                                   \
  }
#define SKIP_PROCESS_TYPE_DEFAULT
#if !defined(MOZ_ENABLE_FORKSERVER)
#  define SKIP_PROCESS_TYPE_FORKSERVER
#endif
#if !defined(ENABLE_TESTS)
#  define SKIP_PROCESS_TYPE_IPDLUNITTEST
#endif
#include "mozilla/GeckoProcessTypes.h"
#undef SKIP_PROCESS_TYPE_IPDLUNITTEST
#undef SKIP_PROCESS_TYPE_FORKSERVER
#undef SKIP_PROCESS_TYPE_DEFAULT
#undef GECKO_PROCESS_TYPE

  MOZ_CRASH("aProcessTypeString is not valid.");
}

void SetGeckoChildID(const char* aGeckoChildIDString) {
  sGeckoChildID = atoi(aGeckoChildIDString);

  if (sGeckoChildID <= 0) {
    MOZ_CRASH("aGeckoChildIDString is not valid.");
  }
}

}  // namespace mozilla

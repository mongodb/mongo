/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IPC_PROCESSTYPE_H_
#define IPC_PROCESSTYPE_H_

#include "mozilla/Attributes.h"
#include "mozilla/Types.h"

// This enum is not dense.  See GeckoProcessTypes.h for details.
enum GeckoProcessType {
#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  GeckoProcessType_##enum_name = (enum_value),
#include "mozilla/GeckoProcessTypes.h"
#undef GECKO_PROCESS_TYPE
  GeckoProcessType_End,
  GeckoProcessType_Invalid = GeckoProcessType_End
};

namespace mozilla {
namespace startup {
extern MFBT_DATA GeckoProcessType sChildProcessType;
}  // namespace startup

/**
 * @return the GeckoProcessType of the current process.
 */
MOZ_ALWAYS_INLINE GeckoProcessType GetGeckoProcessType() {
  return startup::sChildProcessType;
}

/**
 * Set the gecko process type based on a null-terminated byte string.
 */
MFBT_API void SetGeckoProcessType(const char* aProcessTypeString);

}  // namespace mozilla

#endif  // IPC_PROCESSTYPE_H_

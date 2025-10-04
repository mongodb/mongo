/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsProcessMitigations_h
#define mozilla_WindowsProcessMitigations_h

#include "mozilla/Types.h"

namespace mozilla {

MFBT_API bool IsWin32kLockedDown();
MFBT_API void SetWin32kLockedDownInPolicy();
MFBT_API bool IsDynamicCodeDisabled();
MFBT_API bool IsEafPlusEnabled();
MFBT_API bool IsUserShadowStackEnabled();

}  // namespace mozilla

#endif  // mozilla_WindowsProcessMitigations_h

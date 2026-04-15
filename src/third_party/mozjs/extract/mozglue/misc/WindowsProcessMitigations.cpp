/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/WindowsProcessMitigations.h"

#include <processthreadsapi.h>

#include "mozilla/Assertions.h"
#include "mozilla/DynamicallyLinkedFunctionPtr.h"
#include "mozilla/WindowsVersion.h"

static_assert(sizeof(PROCESS_MITIGATION_DYNAMIC_CODE_POLICY) == 4);

namespace mozilla {

static decltype(&::GetProcessMitigationPolicy)
FetchGetProcessMitigationPolicyFunc() {
  static const StaticDynamicallyLinkedFunctionPtr<
      decltype(&::GetProcessMitigationPolicy)>
      pGetProcessMitigationPolicy(L"kernel32.dll",
                                  "GetProcessMitigationPolicy");
  return pGetProcessMitigationPolicy;
}

static bool sWin32kLockedDownInPolicy = false;

MFBT_API bool IsWin32kLockedDown() {
  static bool sWin32kLockedDown = []() {
    auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();

    PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY polInfo;
    if (!pGetProcessMitigationPolicy ||
        !pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                     ProcessSystemCallDisablePolicy, &polInfo,
                                     sizeof(polInfo))) {
      // We failed to get pointer to GetProcessMitigationPolicy or the call
      // to it failed, so just return what the sandbox policy says.
      return sWin32kLockedDownInPolicy;
    }

    return !!polInfo.DisallowWin32kSystemCalls;
  }();

  return sWin32kLockedDown;
}

MFBT_API void SetWin32kLockedDownInPolicy() {
  sWin32kLockedDownInPolicy = true;
}

MFBT_API bool IsDynamicCodeDisabled() {
  auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();
  if (!pGetProcessMitigationPolicy) {
    return false;
  }

  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY polInfo;
  if (!pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessDynamicCodePolicy, &polInfo,
                                   sizeof(polInfo))) {
    return false;
  }

  return polInfo.ProhibitDynamicCode;
}

MFBT_API bool IsEafPlusEnabled() {
  auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();
  if (!pGetProcessMitigationPolicy) {
    return false;
  }

  PROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY polInfo;
  if (!pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessPayloadRestrictionPolicy, &polInfo,
                                   sizeof(polInfo))) {
    return false;
  }

  return polInfo.EnableExportAddressFilterPlus;
}

MFBT_API bool IsUserShadowStackEnabled() {
  auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();
  if (!pGetProcessMitigationPolicy) {
    return false;
  }

  PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY polInfo;
  if (!pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessUserShadowStackPolicy, &polInfo,
                                   sizeof(polInfo))) {
    return false;
  }

  return polInfo.EnableUserShadowStack;
}

MFBT_API bool IsPreferLoadFromSystem32Available() {
  return mozilla::IsWin10AnniversaryUpdateOrLater();
}

MFBT_API bool IsPreferLoadFromSystem32Enabled() {
  auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();
  if (!pGetProcessMitigationPolicy) {
    return false;
  }

  PROCESS_MITIGATION_IMAGE_LOAD_POLICY imgLoadPol{};
  if (!pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessImageLoadPolicy, &imgLoadPol,
                                   sizeof(imgLoadPol))) {
    return false;
  }

  return imgLoadPol.PreferSystem32Images;
}

MFBT_API bool EnablePreferLoadFromSystem32() {
  auto pGetProcessMitigationPolicy = FetchGetProcessMitigationPolicyFunc();
  if (!pGetProcessMitigationPolicy) {
    return false;
  }

  static const mozilla::StaticDynamicallyLinkedFunctionPtr<
      decltype(&::SetProcessMitigationPolicy)>
      pSetProcessMitigationPolicy(L"kernel32.dll",
                                  "SetProcessMitigationPolicy");
  if (!pSetProcessMitigationPolicy) {
    return false;
  }

  PROCESS_MITIGATION_IMAGE_LOAD_POLICY imgLoadPol{};
  if (!pGetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessImageLoadPolicy, &imgLoadPol,
                                   sizeof(imgLoadPol))) {
    return false;
  }

  if (imgLoadPol.PreferSystem32Images) {
    return true;
  }

  imgLoadPol.PreferSystem32Images = 1;
  return pSetProcessMitigationPolicy(ProcessImageLoadPolicy, &imgLoadPol,
                                     sizeof(imgLoadPol));
}

}  // namespace mozilla

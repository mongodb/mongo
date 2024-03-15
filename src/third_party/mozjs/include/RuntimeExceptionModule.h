/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef RUNTIMEEXCEPTIONMODULE_H_
#define RUNTIMEEXCEPTIONMODULE_H_

#include "mozilla/Types.h"

namespace CrashReporter {

MFBT_API void RegisterRuntimeExceptionModule();

MFBT_API void UnregisterRuntimeExceptionModule();

}  // namespace CrashReporter

#endif  // RUNTIMEEXCEPTIONMODULE_H_

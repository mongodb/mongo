/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedScriptDataTableHolder.h"

#include "vm/MutexIDs.h"  // mutexid

using namespace js;

MOZ_RUNINIT js::Mutex AutoLockGlobalScriptData::mutex_(
    mutexid::SharedImmutableScriptData);

AutoLockGlobalScriptData::AutoLockGlobalScriptData() { mutex_.lock(); }

AutoLockGlobalScriptData::~AutoLockGlobalScriptData() { mutex_.unlock(); }

MOZ_RUNINIT SharedScriptDataTableHolder js::globalSharedScriptDataTableHolder;

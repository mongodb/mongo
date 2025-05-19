/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Prefs.h"

#include "js/Initialization.h"
#include "vm/Runtime.h"

// Set all static JS::Prefs fields to the default pref values.
JS_PREF_CLASS_FIELDS_INIT;

#ifdef DEBUG
// static
void JS::Prefs::assertCanSetStartupPref() {
  MOZ_ASSERT(detail::libraryInitState == detail::InitState::Uninitialized,
             "startup prefs must be set before calling JS_Init");
  MOZ_ASSERT(!JSRuntime::hasLiveRuntimes(),
             "startup prefs must be set before creating a JSContext");
}
#endif

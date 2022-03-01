/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/friend/UsageStatistics.h"

#include "vm/JSContext.h"  // JSContext
#include "vm/Runtime.h"    // JSRuntime

void JS_SetAccumulateTelemetryCallback(
    JSContext* cx, JSAccumulateTelemetryDataCallback callback) {
  cx->runtime()->setTelemetryCallback(cx->runtime(), callback);
}

void JS_SetSetUseCounterCallback(JSContext* cx,
                                 JSSetUseCounterCallback callback) {
  cx->runtime()->setUseCounterCallback(cx->runtime(), callback);
}

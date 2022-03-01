/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Value.h"

#include "mozilla/Assertions.h"

#include <inttypes.h>

static const JS::Value JSVAL_NULL =
    JS::Value::fromTagAndPayload(JSVAL_TAG_NULL, 0);
static const JS::Value JSVAL_FALSE =
    JS::Value::fromTagAndPayload(JSVAL_TAG_BOOLEAN, false);
static const JS::Value JSVAL_TRUE =
    JS::Value::fromTagAndPayload(JSVAL_TAG_BOOLEAN, true);
static const JS::Value JSVAL_VOID =
    JS::Value::fromTagAndPayload(JSVAL_TAG_UNDEFINED, 0);
static const mozilla::Maybe<JS::Value> JSVAL_NOTHING;

namespace JS {

const HandleValue NullHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_NULL);
const HandleValue UndefinedHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_VOID);
const HandleValue TrueHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_TRUE);
const HandleValue FalseHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_FALSE);
const Handle<mozilla::Maybe<Value>> NothingHandleValue =
    Handle<mozilla::Maybe<Value>>::fromMarkedLocation(&JSVAL_NOTHING);

}  // namespace JS

void js::ReportBadValueTypeAndCrash(const JS::Value& value) {
  MOZ_CRASH_UNSAFE_PRINTF("JS::Value has illegal type: 0x%" PRIx64,
                          value.asRawBits());
}

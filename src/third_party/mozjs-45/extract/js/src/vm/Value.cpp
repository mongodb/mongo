/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Value.h"

static const JS::Value JSVAL_NULL  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_NULL,      0));
static const JS::Value JSVAL_FALSE = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_BOOLEAN,   false));
static const JS::Value JSVAL_TRUE  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_BOOLEAN,   true));
static const JS::Value JSVAL_VOID  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_UNDEFINED, 0));

namespace JS {

const HandleValue NullHandleValue = HandleValue::fromMarkedLocation(&JSVAL_NULL);
const HandleValue UndefinedHandleValue = HandleValue::fromMarkedLocation(&JSVAL_VOID);
const HandleValue TrueHandleValue = HandleValue::fromMarkedLocation(&JSVAL_TRUE);
const HandleValue FalseHandleValue = HandleValue::fromMarkedLocation(&JSVAL_FALSE);

} // namespace JS

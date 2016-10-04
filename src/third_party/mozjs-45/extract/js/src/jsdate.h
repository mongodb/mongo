/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS Date class interface.
 */

#ifndef jsdate_h
#define jsdate_h

#include "jstypes.h"

#include "js/Date.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "vm/DateTime.h"

namespace js {

/*
 * These functions provide a C interface to the date/time object
 */

/*
 * Construct a new Date Object from a time value given in milliseconds UTC
 * since the epoch.
 */
extern JSObject*
NewDateObjectMsec(JSContext* cx, JS::ClippedTime t, JS::HandleObject proto = nullptr);

/*
 * Construct a new Date Object from an exploded local time value.
 *
 * Assert that mon < 12 to help catch off-by-one user errors, which are common
 * due to the 0-based month numbering copied into JS from Java (java.util.Date
 * in 1995).
 */
extern JS_FRIEND_API(JSObject*)
NewDateObject(JSContext* cx, int year, int mon, int mday,
              int hour, int min, int sec);

/* Date constructor native. Exposed only so the JIT can know its address. */
bool
DateConstructor(JSContext* cx, unsigned argc, JS::Value* vp);

/* Date methods exposed so they can be installed in the self-hosting global. */
bool
date_now(JSContext* cx, unsigned argc, JS::Value* vp);

bool
date_valueOf(JSContext* cx, unsigned argc, JS::Value* vp);

} /* namespace js */

#endif /* jsdate_h */

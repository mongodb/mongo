/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_RelativeTimeFormat_h
#define builtin_intl_RelativeTimeFormat_h

#include "mozilla/Attributes.h"

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

namespace js {

class FreeOp;

class RelativeTimeFormatObject : public NativeObject
{
  public:
    static const Class class_;

    static constexpr uint32_t INTERNALS_SLOT = 0;
    static constexpr uint32_t URELATIVE_TIME_FORMAT_SLOT = 1;
    static constexpr uint32_t SLOT_COUNT = 2;

    static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                  "INTERNALS_SLOT must match self-hosting define for internals object slot");

  private:
    static const ClassOps classOps_;

    static void finalize(FreeOp* fop, JSObject* obj);
};

extern JSObject*
CreateRelativeTimeFormatPrototype(JSContext* cx, JS::Handle<JSObject*> Intl,
                                  JS::Handle<GlobalObject*> global);

/**
 * Returns an object indicating the supported locales for relative time format
 * by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_RelativeTimeFormat_availableLocales()
 */
extern MOZ_MUST_USE bool
intl_RelativeTimeFormat_availableLocales(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns a relative time as a string formatted according to the effective
 * locale and the formatting options of the given RelativeTimeFormat.
 *
 * t should be a number representing a number to be formatted.
 * unit should be "second", "minute", "hour", "day", "week", "month", "quarter", or "year".
 * numeric should be "always" or "auto".
 *
 * Usage: formatted = intl_FormatRelativeTime(relativeTimeFormat, t, unit, numeric)
 */
extern MOZ_MUST_USE bool
intl_FormatRelativeTime(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif /* builtin_intl_RelativeTimeFormat_h */

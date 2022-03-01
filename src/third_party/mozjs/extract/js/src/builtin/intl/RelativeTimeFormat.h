/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_RelativeTimeFormat_h
#define builtin_intl_RelativeTimeFormat_h

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "gc/Barrier.h"
#include "js/Class.h"
#include "vm/NativeObject.h"
#include "vm/Runtime.h"

struct UFormattedValue;
struct URelativeDateTimeFormatter;

namespace js {

class RelativeTimeFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t URELATIVE_TIME_FORMAT_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for URelativeDateTimeFormatter (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 278;

  URelativeDateTimeFormatter* getRelativeDateTimeFormatter() const {
    const auto& slot = getFixedSlot(URELATIVE_TIME_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<URelativeDateTimeFormatter*>(slot.toPrivate());
  }

  void setRelativeDateTimeFormatter(URelativeDateTimeFormatter* rtf) {
    setFixedSlot(URELATIVE_TIME_FORMAT_SLOT, PrivateValue(rtf));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JSFreeOp* fop, JSObject* obj);
};

/**
 * Returns a relative time as a string formatted according to the effective
 * locale and the formatting options of the given RelativeTimeFormat.
 *
 * |t| should be a number representing a number to be formatted.
 * |unit| should be "second", "minute", "hour", "day", "week", "month",
 *                  "quarter", or "year".
 * |numeric| should be "always" or "auto".
 *
 * Usage: formatted = intl_FormatRelativeTime(relativeTimeFormat, t,
 *                                            unit, numeric, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatRelativeTime(JSContext* cx, unsigned argc,
                                                  JS::Value* vp);

namespace intl {

using FieldType = js::ImmutablePropertyNamePtr JSAtomState::*;

[[nodiscard]] bool FormattedRelativeTimeToParts(
    JSContext* cx, const UFormattedValue* formattedValue, double timeValue,
    FieldType relativeTimeUnit, MutableHandleValue result);

}  // namespace intl
}  // namespace js

#endif /* builtin_intl_RelativeTimeFormat_h */

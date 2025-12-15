/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DurationFormat_h
#define builtin_intl_DurationFormat_h

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

namespace js {

class DurationFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t SLOT_COUNT = 1;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

 private:
  static const ClassSpec classSpec_;
};

/**
 * Returns the time separator string for the given locale and numbering system.
 *
 * Usage: separator = intl_GetTimeSeparator(locale, numberingSystem)
 */
[[nodiscard]] extern bool intl_GetTimeSeparator(JSContext* cx, unsigned argc,
                                                Value* vp);

/**
 * `toLocaleString` implementation for Temporal.Duration objects.
 */
[[nodiscard]] extern bool TemporalDurationToLocaleString(
    JSContext* cx, const JS::CallArgs& args);

}  // namespace js

#endif /* builtin_intl_DurationFormat_h */

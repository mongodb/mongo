/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_ListFormat_h
#define builtin_intl_ListFormat_h

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "js/TypeDecls.h"
#include "vm/NativeObject.h"

namespace mozilla::intl {
class ListFormat;
}  // namespace mozilla::intl

namespace js {

class ListFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t LIST_FORMAT_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UListFormatter (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 24;

  mozilla::intl::ListFormat* getListFormatSlot() const {
    const auto& slot = getFixedSlot(LIST_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::ListFormat*>(slot.toPrivate());
  }

  void setListFormatSlot(mozilla::intl::ListFormat* format) {
    setFixedSlot(LIST_FORMAT_SLOT, PrivateValue(format));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

/**
 * Returns a string representing the array of string values |list| according to
 * the effective locale and the formatting options of the given ListFormat.
 *
 * Usage: formatted = intl_FormatList(listFormat, list, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatList(JSContext* cx, unsigned argc,
                                          Value* vp);

}  // namespace js

#endif /* builtin_intl_ListFormat_h */

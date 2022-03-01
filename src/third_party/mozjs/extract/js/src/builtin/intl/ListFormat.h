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
#include "js/RootingAPI.h"
#include "vm/NativeObject.h"

class JSFreeOp;
struct UListFormatter;

namespace js {

class ListFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t ULIST_FORMATTER_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UListFormatter (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 24;

  UListFormatter* getListFormatter() const {
    const auto& slot = getFixedSlot(ULIST_FORMATTER_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<UListFormatter*>(slot.toPrivate());
  }

  void setListFormatter(UListFormatter* formatter) {
    setFixedSlot(ULIST_FORMATTER_SLOT, PrivateValue(formatter));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JSFreeOp* fop, JSObject* obj);
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

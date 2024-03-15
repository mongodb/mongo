/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DisplayNames_h
#define builtin_intl_DisplayNames_h

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"  // JSClass, JSClassOps, js::ClassSpec
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

struct JS_PUBLIC_API JSContext;

namespace mozilla::intl {
class DisplayNames;
}

namespace js {
struct ClassSpec;

class DisplayNamesObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t LOCALE_DISPLAY_NAMES_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 3;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for ULocaleDisplayNames (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 1238;

  mozilla::intl::DisplayNames* getDisplayNames() const {
    const auto& slot = getFixedSlot(LOCALE_DISPLAY_NAMES_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::DisplayNames*>(slot.toPrivate());
  }

  void setDisplayNames(mozilla::intl::DisplayNames* displayNames) {
    setFixedSlot(LOCALE_DISPLAY_NAMES_SLOT, PrivateValue(displayNames));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

/**
 * Return the display name for the requested code or undefined if no applicable
 * display name was found.
 *
 * Usage: result = intl_ComputeDisplayName(displayNames, locale, calendar,
 *                                         style, languageDisplay, fallback,
 *                                         type, code)
 */
[[nodiscard]] extern bool intl_ComputeDisplayName(JSContext* cx, unsigned argc,
                                                  Value* vp);

}  // namespace js

#endif /* builtin_intl_DisplayNames_h */

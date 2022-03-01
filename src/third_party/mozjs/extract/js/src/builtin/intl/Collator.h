/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_Collator_h
#define builtin_intl_Collator_h

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

struct UCollator;

namespace js {

/******************** Collator ********************/

class CollatorObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t UCOLLATOR_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UCollator (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 1128;

  UCollator* getCollator() const {
    const auto& slot = getFixedSlot(UCOLLATOR_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<UCollator*>(slot.toPrivate());
  }

  void setCollator(UCollator* collator) {
    setFixedSlot(UCOLLATOR_SLOT, PrivateValue(collator));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JSFreeOp* fop, JSObject* obj);
};

/**
 * Returns a new instance of the standard built-in Collator constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: collator = intl_Collator(locales, options)
 */
[[nodiscard]] extern bool intl_Collator(JSContext* cx, unsigned argc,
                                        JS::Value* vp);

/**
 * Returns an array with the collation type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * collations supported for the given locale. "standard" and "search" are
 * excluded.
 *
 * Usage: collations = intl_availableCollations(locale)
 */
[[nodiscard]] extern bool intl_availableCollations(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

/**
 * Compares x and y (which must be String values), and returns a number less
 * than 0 if x < y, 0 if x = y, or a number greater than 0 if x > y according
 * to the sort order for the locale and collation options of the given
 * Collator.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.2.
 *
 * Usage: result = intl_CompareStrings(collator, x, y)
 */
[[nodiscard]] extern bool intl_CompareStrings(JSContext* cx, unsigned argc,
                                              JS::Value* vp);

/**
 * Returns true if the given locale sorts upper-case before lower-case
 * characters.
 *
 * Usage: result = intl_isUpperCaseFirst(locale)
 */
[[nodiscard]] extern bool intl_isUpperCaseFirst(JSContext* cx, unsigned argc,
                                                JS::Value* vp);

}  // namespace js

#endif /* builtin_intl_Collator_h */

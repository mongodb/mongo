/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_PluralRules_h
#define builtin_intl_PluralRules_h

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

namespace mozilla::intl {
class PluralRules;
}

namespace js {

class PluralRulesObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t PLURAL_RULES_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UPluralRules (see IcuMemoryUsage).
  // Includes usage for UNumberFormat and UNumberRangeFormatter since our
  // PluralRules implementations contains a NumberFormat and a NumberRangeFormat
  // object.
  static constexpr size_t UPluralRulesEstimatedMemoryUse = 5736;

  mozilla::intl::PluralRules* getPluralRules() const {
    const auto& slot = getFixedSlot(PLURAL_RULES_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::PluralRules*>(slot.toPrivate());
  }

  void setPluralRules(mozilla::intl::PluralRules* pluralRules) {
    setFixedSlot(PLURAL_RULES_SLOT, PrivateValue(pluralRules));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

/**
 * Returns a plural rule for the number x according to the effective
 * locale and the formatting options of the given PluralRules.
 *
 * A plural rule is a grammatical category that expresses count distinctions
 * (such as "one", "two", "few" etc.).
 *
 * Usage: rule = intl_SelectPluralRule(pluralRules, x)
 */
[[nodiscard]] extern bool intl_SelectPluralRule(JSContext* cx, unsigned argc,
                                                JS::Value* vp);

/**
 * Returns a plural rule for the number range «x - y» according to the effective
 * locale and the formatting options of the given PluralRules.
 *
 * A plural rule is a grammatical category that expresses count distinctions
 * (such as "one", "two", "few" etc.).
 *
 * Usage: rule = intl_SelectPluralRuleRange(pluralRules, x, y)
 */
[[nodiscard]] extern bool intl_SelectPluralRuleRange(JSContext* cx,
                                                     unsigned argc,
                                                     JS::Value* vp);

/**
 * Returns an array of plural rules categories for a given pluralRules object.
 *
 * Usage: categories = intl_GetPluralCategories(pluralRules)
 *
 * Example:
 *
 * pluralRules = new Intl.PluralRules('pl', {type: 'cardinal'});
 * intl_getPluralCategories(pluralRules); // ['one', 'few', 'many', 'other']
 */
[[nodiscard]] extern bool intl_GetPluralCategories(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

}  // namespace js

#endif /* builtin_intl_PluralRules_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_PluralRules_h
#define builtin_intl_PluralRules_h

#include "mozilla/Attributes.h"

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "js/RootingAPI.h"
#include "vm/NativeObject.h"

namespace js {

class FreeOp;

class PluralRulesObject : public NativeObject
{
  public:
    static const Class class_;

    static constexpr uint32_t INTERNALS_SLOT = 0;
    static constexpr uint32_t UPLURAL_RULES_SLOT = 1;
    static constexpr uint32_t UNUMBER_FORMAT_SLOT = 2;
    static constexpr uint32_t SLOT_COUNT = 3;

    static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                  "INTERNALS_SLOT must match self-hosting define for internals object slot");

  private:
    static const ClassOps classOps_;

    static void finalize(FreeOp* fop, JSObject* obj);
};

extern JSObject*
CreatePluralRulesPrototype(JSContext* cx, JS::Handle<JSObject*> Intl,
                           JS::Handle<GlobalObject*> global);

/**
 * Returns an object indicating the supported locales for plural rules
 * by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_PluralRules_availableLocales()
 */
extern MOZ_MUST_USE bool
intl_PluralRules_availableLocales(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns a plural rule for the number x according to the effective
 * locale and the formatting options of the given PluralRules.
 *
 * A plural rule is a grammatical category that expresses count distinctions
 * (such as "one", "two", "few" etc.).
 *
 * Usage: rule = intl_SelectPluralRule(pluralRules, x)
 */
extern MOZ_MUST_USE bool
intl_SelectPluralRule(JSContext* cx, unsigned argc, JS::Value* vp);

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
extern MOZ_MUST_USE bool
intl_GetPluralCategories(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif /* builtin_intl_PluralRules_h */

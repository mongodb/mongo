/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_Collator_h
#define builtin_intl_Collator_h

#include "mozilla/Attributes.h"

#include <stdint.h>

#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

namespace js {

class FreeOp;
class GlobalObject;

/******************** Collator ********************/

class CollatorObject : public NativeObject
{
  public:
    static const Class class_;

    static constexpr uint32_t INTERNALS_SLOT = 0;
    static constexpr uint32_t UCOLLATOR_SLOT = 1;
    static constexpr uint32_t SLOT_COUNT = 2;

    static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                  "INTERNALS_SLOT must match self-hosting define for internals object slot");

  private:
    static const ClassOps classOps_;

    static void finalize(FreeOp* fop, JSObject* obj);
};

extern JSObject*
CreateCollatorPrototype(JSContext* cx, JS::Handle<JSObject*> Intl,
                        JS::Handle<GlobalObject*> global);

/**
 * Returns a new instance of the standard built-in Collator constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: collator = intl_Collator(locales, options)
 */
extern MOZ_MUST_USE bool
intl_Collator(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns an object indicating the supported locales for collation
 * by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_Collator_availableLocales()
 */
extern MOZ_MUST_USE bool
intl_Collator_availableLocales(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns an array with the collation type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * collations supported for the given locale. "standard" and "search" are
 * excluded.
 *
 * Usage: collations = intl_availableCollations(locale)
 */
extern MOZ_MUST_USE bool
intl_availableCollations(JSContext* cx, unsigned argc, JS::Value* vp);

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
extern MOZ_MUST_USE bool
intl_CompareStrings(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns true if the given locale sorts upper-case before lower-case
 * characters.
 *
 * Usage: result = intl_isUpperCaseFirst(locale)
 */
extern MOZ_MUST_USE bool
intl_isUpperCaseFirst(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif /* builtin_intl_Collator_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Poor man's reflection for enumerations. */

#ifndef mozilla_DefineEnum_h
#define mozilla_DefineEnum_h

#include <stddef.h>  // for size_t

#include "mozilla/MacroArgs.h"     // for MOZ_ARG_COUNT
#include "mozilla/MacroForEach.h"  // for MOZ_FOR_EACH

/**
 * MOZ_UNWRAP_ARGS is a helper macro that unwraps a list of comma-separated
 * items enclosed in parentheses, to yield just the items.
 *
 * Usage: |MOZ_UNWRAP_ARGS foo| (note the absence of parentheses in the
 * invocation), where |foo| is a parenthesis-enclosed list.
 * For exampe if |foo| is |(3, 4, 5)|, then the expansion is just |3, 4, 5|.
 */
#define MOZ_UNWRAP_ARGS(...) __VA_ARGS__

/**
 * MOZ_DEFINE_ENUM(aEnumName, aEnumerators) is a macro that allows
 * simultaneously defining an enumeration named |aEnumName|, and a constant
 * that stores the number of enumerators it has.
 *
 * The motivation is to allow the enumeration to evolve over time without
 * either having to manually keep such a constant up to date, or having to
 * add a special "sentinel" enumerator for this purpose. (While adding a
 * "sentinel" enumerator is trivial, it causes headaches with "switch"
 * statements. We often try to write "switch" statements whose cases exhaust
 * the enumerators and don't have a "default" case, so that if a new
 * enumerator is added and we forget to handle it in the "switch", the
 * compiler points it out. But this means we need to explicitly handle the
 * sentinel in every "switch".)
 *
 * |aEnumerators| is expected to be a comma-separated list of enumerators,
 * enclosed in parentheses. The enumerators may NOT have associated
 * initializers (an attempt to have one will result in a compiler error).
 * This ensures that the enumerator values are in the range [0, N), where N
 * is the number of enumerators.
 *
 * The list of enumerators cannot contain a trailing comma. This is a
 * limitation of MOZ_FOR_EACH, which we use in the implementation; if
 * MOZ_FOR_EACH supported trailing commas, we could too.
 *
 * The generated constant has the name "k" + |aEnumName| + "Count", and type
 * "size_t". The enumeration and the constant are both defined in the scope
 * in which the macro is invoked.
 *
 * For convenience, a constant of the enumeration type named
 * "kHighest" + |aEnumName| is also defined, whose value is the highest
 * valid enumerator, assuming the enumerators have contiguous values starting
 * from 0.
 *
 * Invocation of the macro may be followed by a semicolon, if one prefers a
 * more declaration-like syntax.
 *
 * Example invocation:
 *   MOZ_DEFINE_ENUM(MyEnum, (Foo, Bar, Baz));
 *
 * This expands to:
 *   enum MyEnum { Foo, Bar, Baz };
 *   constexpr size_t kMyEnumCount = 3;
 *   constexpr MyEnum kHighestMyEnum = MyEnum(kMyEnumCount - 1);
 *   // some static_asserts to ensure the values are in the range [0, 3)
 *
 * The macro also has several variants:
 *
 *    - A |_CLASS| variant, which generates an |enum class| instead of
 *      a plain enum.
 *
 *    - A |_WITH_BASE| variant which generates an enum with a specified
 *      underlying ("base") type, which is provided as an additional
 *      argument in second position.
 *
 *    - An |_AT_CLASS_SCOPE| variant, designed for enumerations defined
 *      at class scope. For these, the generated constants are static,
 *      and have names prefixed with "s" instead of "k" as per
 *      naming convention.
 *
 *  (and combinations of these).
 */

/*
 * A helper macro for asserting that an enumerator does not have an initializer.
 *
 * The static_assert and the comparison to 0 are just scaffolding; the
 * important part is forming the expression |aEnumName::aEnumeratorDecl|.
 *
 * If |aEnumeratorDecl| is just the enumerator name without an identifier,
 * this expression compiles fine. However, if |aEnumeratorDecl| includes an
 * initializer, as in |eEnumerator = initializer|, then this will fail to
 * compile in expression context, since |eEnumerator| is not an lvalue.
 *
 * (The static_assert itself should always pass in the absence of the above
 * error, since you can't get a negative enumerator value without having
 * an initializer somewhere. It just provides a place to put the expression
 * we want to form.)
 */
#define MOZ_ASSERT_ENUMERATOR_HAS_NO_INITIALIZER(aEnumName, aEnumeratorDecl) \
  static_assert((aEnumName::aEnumeratorDecl) >= aEnumName(0), \
                "MOZ_DEFINE_ENUM does not allow enumerators to have initializers");

#define MOZ_DEFINE_ENUM_IMPL(aEnumName, aClassSpec, aBaseSpec, aEnumerators)  \
  enum aClassSpec aEnumName aBaseSpec { MOZ_UNWRAP_ARGS aEnumerators };  \
  constexpr size_t k##aEnumName##Count = MOZ_ARG_COUNT aEnumerators;  \
  constexpr aEnumName k##Highest##aEnumName = aEnumName(k##aEnumName##Count - 1);  \
  MOZ_FOR_EACH(MOZ_ASSERT_ENUMERATOR_HAS_NO_INITIALIZER, (aEnumName,), aEnumerators)

#define MOZ_DEFINE_ENUM(aEnumName, aEnumerators)  \
  MOZ_DEFINE_ENUM_IMPL(aEnumName, , , aEnumerators)

#define MOZ_DEFINE_ENUM_WITH_BASE(aEnumName, aBaseName, aEnumerators) \
  MOZ_DEFINE_ENUM_IMPL(aEnumName, , : aBaseName, aEnumerators)

#define MOZ_DEFINE_ENUM_CLASS(aEnumName, aEnumerators)  \
  MOZ_DEFINE_ENUM_IMPL(aEnumName, class, , aEnumerators)

#define MOZ_DEFINE_ENUM_CLASS_WITH_BASE(aEnumName, aBaseName, aEnumerators) \
  MOZ_DEFINE_ENUM_IMPL(aEnumName, class, : aBaseName, aEnumerators)

#define MOZ_DEFINE_ENUM_AT_CLASS_SCOPE_IMPL(aEnumName, aClassSpec, aBaseSpec, aEnumerators) \
  enum aClassSpec aEnumName aBaseSpec { MOZ_UNWRAP_ARGS aEnumerators };  \
  constexpr static size_t s##aEnumName##Count = MOZ_ARG_COUNT aEnumerators; \
  constexpr static aEnumName s##Highest##aEnumName = aEnumName(s##aEnumName##Count - 1); \
  MOZ_FOR_EACH(MOZ_ASSERT_ENUMERATOR_HAS_NO_INITIALIZER, (aEnumName,), aEnumerators)

#define MOZ_DEFINE_ENUM_AT_CLASS_SCOPE(aEnumName, aEnumerators) \
  MOZ_DEFINE_ENUM_AT_CLASS_SCOPE_IMPL(aEnumName, , , aEnumerators)

#define MOZ_DEFINE_ENUM_WITH_BASE_AT_CLASS_SCOPE(aEnumName, aBaseName, aEnumerators)  \
  MOZ_DEFINE_ENUM_AT_CLASS_SCOPE_IMPL(aEnumName, , : aBaseName, aEnumerators)

#define MOZ_DEFINE_ENUM_CLASS_AT_CLASS_SCOPE(aEnumName, aEnumerators) \
  MOZ_DEFINE_ENUM_AT_CLASS_SCOPE_IMPL(aEnumName, class, , aEnumerators)

#define MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AT_CLASS_SCOPE(aEnumName, aBaseName, aEnumerators)  \
  MOZ_DEFINE_ENUM_AT_CLASS_SCOPE_IMPL(aEnumName, class, : aBaseName, aEnumerators)

#endif // mozilla_DefineEnum_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Creates a Tainted<> wrapper to enforce data validation before use.
 */

#ifndef mozilla_Tainting_h
#define mozilla_Tainting_h

#include <utility>
#include "mozilla/MacroArgs.h"

namespace mozilla {

template <typename T>
class Tainted;

namespace ipc {
template <typename>
struct IPDLParamTraits;
}

/*
 * The Tainted<> class allows data to be wrapped and considered 'tainted'; which
 * requires explicit validation of the data before it can be used for
 * comparisons or in arithmetic.
 *
 * Tainted<> objects are intended to be passed down callstacks (still in
 * Tainted<> form) to whatever location is appropriate to validate (or complete
 * validation) of the data before finally unwrapping it.
 *
 * Tainting data ensures that validation actually occurs and is not forgotten,
 * increase consideration of validation so it can be as strict as possible, and
 * makes it clear from a code point of view where and what validation is
 * performed.
 */

// ====================================================================
// ====================================================================
/*
 * Simple Tainted<foo> class
 *
 * Class should not support any de-reference or comparison operator and instead
 * force all access to the member variable through the MOZ_VALIDATE macros.
 *
 * While the Coerce() function is publicly accessible on the class, it should
 * only be used by the MOZ_VALIDATE macros, and static analysis will prevent
 * it being used elsewhere.
 */

template <typename T>
class Tainted {
 private:
  T mValue;

 public:
  explicit Tainted() = default;

  template <typename U>
  explicit Tainted(U&& aValue) : mValue(std::forward<U>(aValue)) {}

  T& Coerce() { return this->mValue; }
  const T& Coerce() const { return this->mValue; }

  friend struct mozilla::ipc::IPDLParamTraits<Tainted<T>>;
};

// ====================================================================
// ====================================================================
/*
 * This section contains obscure, non-user-facing C++ to support
 * variable-argument macros.
 */
#define MOZ_TAINT_GLUE(a, b) a b

// We use the same variable name in the nested scope, shadowing the outer
// scope - this allows the user to write the same variable name in the
// macro's condition without using a magic name like 'value'.
//
// We explicitly do not mark it MOZ_MAYBE_UNUSED because the condition
// should always make use of tainted_value, not doing so should cause an
// unused variable warning. That would only happen when we are bypssing
// validation.
//
// The separate bool variable is required to allow condition to be a lambda
// expression; lambdas cannot be placed directly inside ASSERTs.
#define MOZ_VALIDATE_AND_GET_HELPER3(tainted_value, condition, \
                                     assertionstring)          \
  [&]() {                                                      \
    auto& tmp = tainted_value.Coerce();                        \
    auto& tainted_value = tmp;                                 \
    bool test = (condition);                                   \
    MOZ_RELEASE_ASSERT(test, assertionstring);                 \
    return tmp;                                                \
  }()

#define MOZ_VALIDATE_AND_GET_HELPER2(tainted_value, condition)        \
  MOZ_VALIDATE_AND_GET_HELPER3(tainted_value, condition,              \
                               "MOZ_VALIDATE_AND_GET(" #tainted_value \
                               ", " #condition ") has failed")

// ====================================================================
// ====================================================================
/*
 * Macros to validate and un-taint a value.
 *
 * All macros accept the tainted variable as the first argument, and a
 * condition as the second argument. If the condition is satisfied,
 * then the value is considered valid.
 *
 * This file contains documentation and examples for the functions;
 * more usage examples are present in mfbt/tests/gtest/TestTainting.cpp
 */

/*
 * MOZ_VALIDATE_AND_GET is the bread-and-butter validation function.
 * It confirms the value abides by the condition specified and then
 * returns the untainted value.
 *
 * If the condition is not satisified, we RELEASE_ASSERT.
 *
 * Examples:
 *
 *   int bar;
 *   Tainted<int> foo;
 *   int comparisonVariable = 20;
 *
 *   bar = MOZ_VALIDATE_AND_GET(foo, foo < 20);
 *   bar = MOZ_VALIDATE_AND_GET(foo, foo < comparisonVariable);
 *
 * Note that while the comparison of foo < 20 works inside the macro,
 * doing so outside the macro (such as with `if (foo < 20)` will
 * (intentionally) fail during compilation. We do this to ensure that
 * all validation logic is self-contained inside the macro.
 *
 *
 * The macro also supports supplying a custom string to the
 * MOZ_RELEASE_ASSERT. This is strongly encouraged because it
 * provides the author the opportunity to explain by way of an
 * english comment what is happening.
 *
 * Good things to include in the comment:
 *  - What the validation is doing or what it means
 *  - The impact that could occur if validation was bypassed.
 *    e.g. 'This value is used to allocate memory, so sane values
 *          should be enforced.''
 *  - How validation could change in the future to be more or less
 *    restrictive.
 *
 * Example:
 *
 *   bar = MOZ_VALIDATE_AND_GET(
 *    foo, foo < 20,
 *    "foo must be less than 20 because higher values represent decibel"
 *    "levels greater than a a jet engine inside your ear.");
 *
 *
 * The condition can also be a lambda function if you need to
 * define temporary variables or perform more complex validation.
 *
 * Square brackets represent the capture group - local variables
 * can be specified here to capture them and use them inside the
 * lambda. Prefacing the variable with '&' means the variable is
 * captured by-reference. It is typically better to capture
 * variables by reference rather than making them parameters.
 *
 * When using this technique:
 *  - the tainted value must be present and should be captured
 *    by reference. (You could make it a parameter if you wish, but
 *    it's more typing.)
 *  - the entire lambda function must be enclosed in parens
 *    (if you omit this, you might get errors of the form:
 *     'use of undeclared identifier 'MOZ_VALIDATE_AND_GET_HELPER4')
 *
 * Example:
 *
 *   bar = MOZ_VALIDATE_AND_GET(foo, ([&foo, &comparisonVariable]() {
 *           bool intermediateResult = externalFunction(foo);
 *           if (intermediateResult || comparisonVariable < 4) {
 *             return true;
 *           }
 *           return false;
 *         }()));
 *
 *
 * You can also define a lambda external to the macro if you prefer
 * this over a static function.
 *
 * This is possible, and supported, but requires a different syntax.
 * Instead of specifying the tainted value in the capture group [&foo],
 * it must be provided as an argument of the unwrapped type.
 * (The argument name can be anything you choose of course.)
 *
 * Example:
 *
 *   auto lambda1 = [](int foo) {
 *     bool intermediateResult = externalFunction(foo);
 *     if (intermediateResult) {
 *       return true;
 *     }
 *     return false;
 *   };
 *   bar = MOZ_VALIDATE_AND_GET(foo, lambda1(foo));
 *
 *
 * Arguments:
 *   tainted_value - the name of the Tainted<> variable
 *   condition - a comparison involving the tainted value
 *   assertionstring [optional] - A string to include in the RELEASE_ASSERT
 */
#define MOZ_VALIDATE_AND_GET(...)                                            \
  MOZ_TAINT_GLUE(MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_VALIDATE_AND_GET_HELPER, \
                                                __VA_ARGS__),                \
                 (__VA_ARGS__))

/*
 * MOZ_IS_VALID is the other most common use, it allows one to test
 * validity without asserting, for use in a if/else statement.
 *
 * It supports the same lambda behavior, but does not support a
 * comment explaining the validation.
 *
 * Example:
 *
 *   if (MOZ_IS_VALID(foo, foo < 20)) {
 *      ...
 *   }
 *
 *
 * Arguments:
 *   tainted_value - the name of the Tainted<> variable
 *   condition - a comparison involving the tainted value
 */
#define MOZ_IS_VALID(tainted_value, condition) \
  [&]() {                                      \
    auto& tmp = tainted_value.Coerce();        \
    auto& tainted_value = tmp;                 \
    return (condition);                        \
  }()

/*
 * MOZ_VALIDATE_OR is a shortcut that tests validity and if invalid,
 * return an alternate value.
 *
 * Note that the following will not work:
 *   MOZ_RELEASE_ASSERT(MOZ_VALIDATE_OR(foo, foo < 20, 100) == EXPECTED_VALUE);
 *   MOZ_ASSERT(MOZ_VALIDATE_OR(foo, foo < 20, 100) == EXPECTED_VALUE);
 * This is because internally, many MOZ_VALIDATE macros use lambda
 * expressions (for variable shadowing purposes) and lambas cannot be
 * expressions in (potentially) unevaluated operands.
 *
 * Example:
 *
 *   bar = MOZ_VALIDATE_OR(foo, foo < 20, 100);
 *
 *
 * Arguments:
 *   tainted_value - the name of the Tainted<> variable
 *   condition - a comparison involving the tainted value
 *   alternate_value - the value to use if the condition is false
 */
#define MOZ_VALIDATE_OR(tainted_value, condition, alternate_value) \
  (MOZ_IS_VALID(tainted_value, condition) ? tainted_value.Coerce() \
                                          : alternate_value)

/*
 * MOZ_FIND_AND_VALIDATE is for testing validity of a tainted value by comparing
 * it against a list of known safe values. Returns a pointer to the matched
 * safe value or nullptr if none was found.
 *
 * Note that for the comparison the macro will loop over the list and that the
 * current element being tested against is provided as list_item.
 *
 * Example:
 *
 * Tainted<int> aId;
 * NSTArray<Person> list;
 * const Person* foo = MOZ_FIND_AND_VALIDATE(aId, list_item.id == aId, list);
 *
 * // Typically you would do nothing if invalid data is passed:
 * if (MOZ_UNLIKELY(!foo)) {
 *     return;
 * }
 *
 * // Or alternately you can crash on invalid data
 * MOZ_RELEASE_ASSERT(foo != nullptr, "Invalid person id sent from content
 * process.");
 *
 * Arguments:
 *  tainted_value - the name of the Tainted<> variable
 *  condition - a condition involving the tainted value and list_item
 *  validation_list - a list of known safe values to compare against
 */
#define MOZ_FIND_AND_VALIDATE(tainted_value, condition, validation_list) \
  [&]() {                                                                \
    auto& tmp = tainted_value.Coerce();                                  \
    auto& tainted_value = tmp;                                           \
    const auto macro_find_it =                                           \
        std::find_if(validation_list.cbegin(), validation_list.cend(),   \
                     [&](const auto& list_item) { return condition; });  \
    return macro_find_it != validation_list.cend() ? &*macro_find_it     \
                                                   : nullptr;            \
  }()

/*
 * MOZ_NO_VALIDATE allows unsafe removal of the Taint wrapper.
 * A justification string is required to explain why this is acceptable.
 *
 * Example:
 *
 *  bar = MOZ_NO_VALIDATE(
 *    foo,
 *    "Value is used to match against a dictionary key in the parent."
 *    "If there's no key present, there won't be a match."
 *    "There is no risk of grabbing a cross-origin value from the dictionary,"
 *    "because the IPC actor is instatiated per-content-process and the "
 *    "dictionary is not shared between actors.");
 *
 *
 * Arguments:
 *   tainted_value - the name of the Tainted<> variable
 *   justification - a human-understandable string explaining why it is
 *                   permissible to omit validation
 */
#define MOZ_NO_VALIDATE(tainted_value, justification)      \
  [&tainted_value] {                                       \
    static_assert(sizeof(justification) > 3,               \
                  "Must provide a justification string."); \
    return tainted_value.Coerce();                         \
  }()

/*
 TODO:

  - Figure out if there are helpers that would be useful for Strings and
 Principals
  - Write static analysis to enforce invariants:
    - No use of .Coerce() except in the header file.
    - No constant passed to the condition of MOZ_VALIDATE_AND_GET
 */

}  // namespace mozilla

#endif /* mozilla_Tainting_h */

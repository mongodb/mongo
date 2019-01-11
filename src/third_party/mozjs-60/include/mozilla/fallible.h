/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_fallible_h
#define mozilla_fallible_h

#if defined(__cplusplus)

/* Explicit fallible allocation
 *
 * Memory allocation (normally) defaults to abort in case of failed
 * allocation. That is, it never returns NULL, and crashes instead.
 *
 * Code can explicitely request for fallible memory allocation thanks
 * to the declarations below.
 *
 * The typical use of the mozilla::fallible const is with placement new,
 * like the following:
 *
 *     foo = new (mozilla::fallible) Foo();
 *
 * The following forms, or derivatives, are also possible but deprecated:
 *
 *     foo = new ((mozilla::fallible_t())) Foo();
 *
 *     const mozilla::fallible_t fallible = mozilla::fallible_t();
 *     bar = new (f) Bar();
 *
 * It is also possible to declare method overloads with fallible allocation
 * alternatives, like so:
 *
 *     class Foo {
 *     public:
 *       void Method(void *);
 *       void Method(void *, const mozilla::fallible_t&);
 *     };
 *
 *     Foo foo;
 *     foo.Method(nullptr, mozilla::fallible);
 *
 * If that last method call is in a method that itself takes a const
 * fallible_t& argument, it is recommended to propagate that argument
 * instead of using mozilla::fallible:
 *
 *     void Func(Foo &foo, const mozilla::fallible_t& aFallible) {
 *         foo.Method(nullptr, aFallible);
 *     }
 *
 */

#include <new>

namespace mozilla {

using fallible_t = std::nothrow_t;

static const fallible_t& fallible = std::nothrow;

} // namespace mozilla

#endif

#endif // mozilla_fallible_h

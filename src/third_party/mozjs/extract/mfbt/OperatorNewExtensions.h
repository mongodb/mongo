/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A version of |operator new| that eschews mandatory null-checks. */

#ifndef mozilla_OperatorNewExtensions_h
#define mozilla_OperatorNewExtensions_h

#include "mozilla/Assertions.h"

// Credit goes to WebKit for this implementation, cf.
// https://bugs.webkit.org/show_bug.cgi?id=74676
namespace mozilla {
enum NotNullTag {
  KnownNotNull,
};
} // namespace mozilla

/*
 * The logic here is a little subtle.  [expr.new] states that if the allocation
 * function being called returns null, then object initialization must not be
 * done, and the entirety of the new expression must return null.  Non-throwing
 * (noexcept) functions are defined to return null to indicate failure.  The
 * standard placement operator new is defined in such a way, and so it requires
 * a null check, even when that null check would be extraneous.  Functions
 * declared without such a specification are defined to throw std::bad_alloc if
 * they fail, and return a non-null pointer otherwise.  We compile without
 * exceptions, so any placement new overload we define that doesn't declare
 * itself as noexcept must therefore avoid generating a null check.  Below is
 * just such an overload.
 *
 * You might think that MOZ_NONNULL might perform the same function, but
 * MOZ_NONNULL isn't supported on all of our compilers, and even when it is
 * supported, doesn't work on all the versions we support.  And even keeping
 * those limitations in mind, we can't put MOZ_NONNULL on the global,
 * standardized placement new function in any event.
 *
 * We deliberately don't add MOZ_NONNULL(3) to tag |p| as non-null, to benefit
 * hypothetical static analyzers.  Doing so makes |MOZ_ASSERT(p)|'s internal
 * test vacuous, and some compilers warn about such vacuous tests.
 */
inline void*
operator new(size_t, mozilla::NotNullTag, void* p)
{
  MOZ_ASSERT(p);
  return p;
}

#endif // mozilla_OperatorNewExtensions_h

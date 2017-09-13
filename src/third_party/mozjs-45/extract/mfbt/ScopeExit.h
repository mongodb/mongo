/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* RAII class for executing arbitrary actions at scope end. */

#ifndef mozilla_ScopeExit_h
#define mozilla_ScopeExit_h

/*
 * See http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4189.pdf for a
 * standards-track version of this.
 *
 * Error handling can be complex when various actions need to be performed that
 * need to be undone if an error occurs midway. This can be handled with a
 * collection of boolean state variables and gotos, which can get clunky and
 * error-prone:
 *
 * {
 *   if (!a.setup())
 *       goto fail;
 *   isASetup = true;
 *
 *   if (!b.setup())
 *       goto fail;
 *   isBSetup = true;
 *
 *   ...
 *   return true;
 *
 *   fail:
 *     if (isASetup)
 *         a.teardown();
 *     if (isBSetup)
 *         b.teardown();
 *     return false;
 *  }
 *
 * ScopeExit is a mechanism to simplify this pattern by keeping an RAII guard
 * class that will perform the teardown on destruction, unless released. So the
 * above would become:
 *
 * {
 *   if (!a.setup()) {
 *       return false;
 *   }
 *   auto guardA = MakeScopeExit([&] {
 *       a.teardown();
 *   });
 *
 *   if (!b.setup()) {
 *       return false;
 *   }
 *   auto guardB = MakeScopeExit([&] {
 *       b.teardown();
 *   });
 *
 *   ...
 *   guardA.release();
 *   guardB.release();
 *   return true;
 * }
 *
 * This header provides:
 *
 * - |ScopeExit| - a container for a cleanup call, automically called at the
 *   end of the scope;
 * - |MakeScopeExit| - a convenience function for constructing a |ScopeExit|
 *   with a given cleanup routine, commonly used with a lambda function.
 *
 * Note that the RAII classes defined in this header do _not_ perform any form
 * of reference-counting or garbage-collection. These classes have exactly two
 * behaviors:
 *
 * - if |release()| has not been called, the cleanup is always performed at
 *   the end of the scope;
 * - if |release()| has been called, nothing will happen at the end of the
 *   scope.
 */

#include "mozilla/GuardObjects.h"
#include "mozilla/Move.h"

namespace mozilla {

template <typename ExitFunction>
class MOZ_STACK_CLASS ScopeExit {
  ExitFunction mExitFunction;
  bool mExecuteOnDestruction;
  MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

public:
  explicit ScopeExit(ExitFunction&& cleanup
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
   : mExitFunction(cleanup)
   , mExecuteOnDestruction(true)
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
  }

  ScopeExit(ScopeExit&& rhs)
   : mExitFunction(mozilla::Move(rhs.mExitFunction))
   , mExecuteOnDestruction(rhs.mExecuteOnDestruction)
  {
    rhs.release();
  }

  ~ScopeExit() {
    if (mExecuteOnDestruction) {
      mExitFunction();
    }
  }

  void release() {
    mExecuteOnDestruction = false;
  }

private:
  explicit ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;
};

template <typename ExitFunction>
ScopeExit<ExitFunction>
MakeScopeExit(ExitFunction&& exitFunction)
{
  return ScopeExit<ExitFunction>(mozilla::Move(exitFunction));
}

} /* namespace mozilla */

#endif /* mozilla_ScopeExit_h */

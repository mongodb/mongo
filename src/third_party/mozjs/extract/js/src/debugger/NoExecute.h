/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_NoExecute_h
#define debugger_NoExecute_h

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Attributes.h"  // for MOZ_RAII

#include "NamespaceImports.h"  // for HandleScript
#include "js/Promise.h"        // for JS::AutoDebuggerJobQueueInterruption

namespace js {

class Debugger;
class LeaveDebuggeeNoExecute;

// Prevents all the debuggeee compartments of a given Debugger from executing
// scripts. Attempts to run script will throw an
// instance of Debugger.DebuggeeWouldRun from the topmost locked Debugger's
// compartment.
class MOZ_RAII EnterDebuggeeNoExecute {
  friend class LeaveDebuggeeNoExecute;

  Debugger& dbg_;
  EnterDebuggeeNoExecute** stack_;
  EnterDebuggeeNoExecute* prev_;

  // Non-nullptr when unlocked temporarily by a LeaveDebuggeeNoExecute.
  LeaveDebuggeeNoExecute* unlocked_;

  // When DebuggeeWouldRun is a warning instead of an error, whether we've
  // reported a warning already.
  bool reported_;

 public:
  // Mark execution in dbg's debuggees as forbidden, for the lifetime of this
  // object. Require an AutoDebuggerJobQueueInterruption in scope.
  explicit EnterDebuggeeNoExecute(
      JSContext* cx, Debugger& dbg,
      const JS::AutoDebuggerJobQueueInterruption& adjqiProof);

  ~EnterDebuggeeNoExecute() {
    MOZ_ASSERT(*stack_ == this);
    *stack_ = prev_;
  }

  Debugger& debugger() const { return dbg_; }

#ifdef DEBUG
  static bool isLockedInStack(JSContext* cx, Debugger& dbg);
#endif

  // Given a JSContext entered into a debuggee realm, find the lock
  // that locks it. Returns nullptr if not found.
  static EnterDebuggeeNoExecute* findInStack(JSContext* cx);

  // Given a JSContext entered into a debuggee compartment, report a
  // warning or an error if there is a lock that locks it.
  static bool reportIfFoundInStack(JSContext* cx, HandleScript script);
};

// Given a JSContext entered into a debuggee compartment, if it is in
// an NX section, unlock the topmost EnterDebuggeeNoExecute instance.
//
// Does nothing if debuggee is not in an NX section. For example, this
// situation arises when invocation functions are called without entering
// Debugger code, e.g., calling D.O.p.executeInGlobal or D.O.p.apply.
class MOZ_RAII LeaveDebuggeeNoExecute {
  EnterDebuggeeNoExecute* prevLocked_;

 public:
  explicit LeaveDebuggeeNoExecute(JSContext* cx)
      : prevLocked_(EnterDebuggeeNoExecute::findInStack(cx)) {
    if (prevLocked_) {
      MOZ_ASSERT(!prevLocked_->unlocked_);
      prevLocked_->unlocked_ = this;
    }
  }

  ~LeaveDebuggeeNoExecute() {
    if (prevLocked_) {
      MOZ_ASSERT(prevLocked_->unlocked_ == this);
      prevLocked_->unlocked_ = nullptr;
    }
  }
};

} /* namespace js */

#endif /* debugger_NoExecute_h */

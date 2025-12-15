/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Stack_h
#define js_Stack_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe
#include "mozilla/Variant.h"     // mozilla::Variant

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t, uintptr_t, UINTPTR_MAX
#include <utility>   // std::move

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/NativeStackLimits.h"
#include "js/Principals.h"  // JSPrincipals, JS_HoldPrincipals, JS_DropPrincipals
#include "js/RootingAPI.h"

/**
 * Set the size of the native stack that should not be exceed. To disable
 * stack size checking pass 0.
 *
 * SpiderMonkey allows for a distinction between system code (such as GCs, which
 * may incidentally be triggered by script but are not strictly performed on
 * behalf of such script), trusted script (as determined by
 * JS_SetTrustedPrincipals), and untrusted script. Each kind of code may have a
 * different stack quota, allowing embedders to keep higher-priority machinery
 * running in the face of scripted stack exhaustion by something else.
 *
 * The stack quotas for each kind of code should be monotonically descending,
 * and may be specified with this function. If 0 is passed for a given kind
 * of code, it defaults to the value of the next-highest-priority kind.
 *
 * This function may only be called immediately after the runtime is initialized
 * and before any code is executed and/or interrupts requested.
 */
extern JS_PUBLIC_API void JS_SetNativeStackQuota(
    JSContext* cx, JS::NativeStackSize systemCodeStackSize,
    JS::NativeStackSize trustedScriptStackSize = 0,
    JS::NativeStackSize untrustedScriptStackSize = 0);

namespace js {

enum class StackFormat { SpiderMonkey, V8, Default };

/*
 * Sets the format used for stringifying Error stacks.
 *
 * The default format is StackFormat::SpiderMonkey.  Use StackFormat::V8
 * in order to emulate V8's stack formatting.  StackFormat::Default can't be
 * used here.
 */
extern JS_PUBLIC_API void SetStackFormat(JSContext* cx, StackFormat format);

extern JS_PUBLIC_API StackFormat GetStackFormat(JSContext* cx);

}  // namespace js

namespace JS {

/**
 * Capture all frames.
 */
struct AllFrames {};

/**
 * Capture at most this many frames.
 */
struct MaxFrames {
  uint32_t maxFrames;

  explicit MaxFrames(uint32_t max) : maxFrames(max) { MOZ_ASSERT(max > 0); }
};

/**
 * Capture the first frame with the given principals. By default, do not
 * consider self-hosted frames with the given principals as satisfying the stack
 * capture.
 */
struct JS_PUBLIC_API FirstSubsumedFrame {
  JSContext* cx;
  JSPrincipals* principals;
  bool ignoreSelfHosted;

  /**
   * Use the cx's current compartment's principals.
   */
  explicit FirstSubsumedFrame(JSContext* cx,
                              bool ignoreSelfHostedFrames = true);

  explicit FirstSubsumedFrame(JSContext* ctx, JSPrincipals* p,
                              bool ignoreSelfHostedFrames = true)
      : cx(ctx), principals(p), ignoreSelfHosted(ignoreSelfHostedFrames) {
    if (principals) {
      JS_HoldPrincipals(principals);
    }
  }

  // No copying because we want to avoid holding and dropping principals
  // unnecessarily.
  FirstSubsumedFrame(const FirstSubsumedFrame&) = delete;
  FirstSubsumedFrame& operator=(const FirstSubsumedFrame&) = delete;
  FirstSubsumedFrame& operator=(FirstSubsumedFrame&&) = delete;

  FirstSubsumedFrame(FirstSubsumedFrame&& rhs)
      : principals(rhs.principals), ignoreSelfHosted(rhs.ignoreSelfHosted) {
    MOZ_ASSERT(this != &rhs, "self move disallowed");
    rhs.principals = nullptr;
  }

  ~FirstSubsumedFrame() {
    if (principals) {
      JS_DropPrincipals(cx, principals);
    }
  }
};

using StackCapture = mozilla::Variant<AllFrames, MaxFrames, FirstSubsumedFrame>;

/**
 * Capture the current call stack as a chain of SavedFrame JSObjects, and set
 * |stackp| to the SavedFrame for the youngest stack frame, or nullptr if there
 * are no JS frames on the stack.
 *
 * The |capture| parameter describes the portion of the JS stack to capture:
 *
 *   * |JS::AllFrames|: Capture all frames on the stack.
 *
 *   * |JS::MaxFrames|: Capture no more than |JS::MaxFrames::maxFrames| from the
 *      stack.
 *
 *   * |JS::FirstSubsumedFrame|: Capture the first frame whose principals are
 *     subsumed by |JS::FirstSubsumedFrame::principals|. By default, do not
 *     consider self-hosted frames; this can be controlled via the
 *     |JS::FirstSubsumedFrame::ignoreSelfHosted| flag. Do not capture any async
 *     stack.
 */
extern JS_PUBLIC_API bool CaptureCurrentStack(
    JSContext* cx, MutableHandleObject stackp,
    StackCapture&& capture = StackCapture(AllFrames()),
    HandleObject startAfter = nullptr);

/**
 * Returns true if capturing stack trace data to associate with an asynchronous
 * operation is currently enabled for the current context realm.
 *
 * Users should check this state before capturing a stack that will be passed
 * back to AutoSetAsyncStackForNewCalls later, in order to avoid capturing a
 * stack for async use when we don't actually want to capture it.
 */
extern JS_PUBLIC_API bool IsAsyncStackCaptureEnabledForRealm(JSContext* cx);

/*
 * This is a utility function for preparing an async stack to be used
 * by some other object.  This may be used when you need to treat a
 * given stack trace as an async parent.  If you just need to capture
 * the current stack, async parents and all, use CaptureCurrentStack
 * instead.
 *
 * Here |asyncStack| is the async stack to prepare.  It is copied into
 * |cx|'s current compartment, and the newest frame is given
 * |asyncCause| as its asynchronous cause.  If |maxFrameCount| is
 * |Some(n)|, capture at most the youngest |n| frames.  The
 * new stack object is written to |stackp|.  Returns true on success,
 * or sets an exception and returns |false| on error.
 */
extern JS_PUBLIC_API bool CopyAsyncStack(
    JSContext* cx, HandleObject asyncStack, HandleString asyncCause,
    MutableHandleObject stackp, const mozilla::Maybe<size_t>& maxFrameCount);

/**
 * Given a SavedFrame JSObject stack, stringify it in the same format as
 * Error.prototype.stack. The stringified stack out parameter is placed in the
 * cx's compartment. Defaults to the empty string.
 *
 * The same notes above about SavedFrame accessors applies here as well: cx
 * doesn't need to be in stack's compartment, and stack can be null, a
 * SavedFrame object, or a wrapper (CCW or Xray) around a SavedFrame object.
 * SavedFrames not subsumed by |principals| are skipped.
 *
 * Optional indent parameter specifies the number of white spaces to indent
 * each line.
 */
extern JS_PUBLIC_API bool BuildStackString(
    JSContext* cx, JSPrincipals* principals, HandleObject stack,
    MutableHandleString stringp, size_t indent = 0,
    js::StackFormat stackFormat = js::StackFormat::Default);

}  // namespace JS

#endif  // js_Stack_h

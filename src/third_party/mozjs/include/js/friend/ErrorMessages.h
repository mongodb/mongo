/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * SpiderMonkey internal error numbering and error-formatting functionality
 * (also for warnings).
 *
 * This functionality is moderately stable.  JSErrNum and js::GetErrorMessage
 * are widely used inside SpiderMonkey, and Gecko uses them to produce errors
 * identical to those SpiderMonkey itself would produce, in various situations.
 * However, the set of error numbers is not stable, error number values are not
 * stable, error types are not stable, etc.  Use your own error reporting code
 * if you can.
 */

#ifndef js_friend_ErrorMessages_h
#define js_friend_ErrorMessages_h

#include "jstypes.h"  // JS_PUBLIC_API

struct JSErrorFormatString;

enum JSErrNum {
#define MSG_DEF(name, count, exception, format) name,
#include "js/friend/ErrorNumbers.msg"
#undef MSG_DEF
  JSErr_Limit
};

namespace js {

/**
 * A JSErrorCallback suitable for passing to |JS_ReportErrorNumberASCII| and
 * similar functions in concert with one of the |JSErrNum| error numbers.
 *
 * This function is a function only of |errorNumber|: |userRef| and ambient
 * state have no effect on its behavior.
 */
extern JS_PUBLIC_API const JSErrorFormatString* GetErrorMessage(
    void* userRef, unsigned errorNumber);

}  // namespace js

#endif  // js_friend_ErrorMessages_h

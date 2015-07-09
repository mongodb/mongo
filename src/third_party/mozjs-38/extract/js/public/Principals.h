/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JSPrincipals and related interfaces. */

#ifndef js_Principals_h
#define js_Principals_h

#include "mozilla/Atomics.h"

#include <stdint.h>

#include "jspubtd.h"

struct JSPrincipals {
    /* Don't call "destroy"; use reference counting macros below. */
    mozilla::Atomic<int32_t> refcount;

#ifdef JS_DEBUG
    /* A helper to facilitate principals debugging. */
    uint32_t    debugToken;
#endif

    JSPrincipals() : refcount(0) {}

    void setDebugToken(uint32_t token) {
# ifdef JS_DEBUG
        debugToken = token;
# endif
    }

    /*
     * This is not defined by the JS engine but should be provided by the
     * embedding.
     */
    JS_PUBLIC_API(void) dump();
};

extern JS_PUBLIC_API(void)
JS_HoldPrincipals(JSPrincipals* principals);

extern JS_PUBLIC_API(void)
JS_DropPrincipals(JSRuntime* rt, JSPrincipals* principals);

// Return whether the first principal subsumes the second. The exact meaning of
// 'subsumes' is left up to the browser. Subsumption is checked inside the JS
// engine when determining, e.g., which stack frames to display in a backtrace.
typedef bool
(* JSSubsumesOp)(JSPrincipals* first, JSPrincipals* second);

/*
 * Used to check if a CSP instance wants to disable eval() and friends.
 * See js_CheckCSPPermitsJSAction() in jsobj.
 */
typedef bool
(* JSCSPEvalChecker)(JSContext* cx);

struct JSSecurityCallbacks {
    JSCSPEvalChecker           contentSecurityPolicyAllows;
    JSSubsumesOp               subsumes;
};

extern JS_PUBLIC_API(void)
JS_SetSecurityCallbacks(JSRuntime* rt, const JSSecurityCallbacks* callbacks);

extern JS_PUBLIC_API(const JSSecurityCallbacks*)
JS_GetSecurityCallbacks(JSRuntime* rt);

/*
 * Code running with "trusted" principals will be given a deeper stack
 * allocation than ordinary scripts. This allows trusted script to run after
 * untrusted script has exhausted the stack. This function sets the
 * runtime-wide trusted principal.
 *
 * This principals is not held (via JS_HoldPrincipals/JS_DropPrincipals) since
 * there is no available JSContext. Instead, the caller must ensure that the
 * given principals stays valid for as long as 'rt' may point to it. If the
 * principals would be destroyed before 'rt', JS_SetTrustedPrincipals must be
 * called again, passing nullptr for 'prin'.
 */
extern JS_PUBLIC_API(void)
JS_SetTrustedPrincipals(JSRuntime* rt, const JSPrincipals* prin);

typedef void
(* JSDestroyPrincipalsOp)(JSPrincipals* principals);

/*
 * Initialize the callback that is called to destroy JSPrincipals instance
 * when its reference counter drops to zero. The initialization can be done
 * only once per JS runtime.
 */
extern JS_PUBLIC_API(void)
JS_InitDestroyPrincipalsCallback(JSRuntime* rt, JSDestroyPrincipalsOp destroyPrincipals);

#endif  /* js_Principals_h */

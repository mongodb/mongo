/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JSPrincipals and related interfaces. */

#ifndef js_Principals_h
#define js_Principals_h

#include "mozilla/Atomics.h"

#include <stdint.h>

#include "jstypes.h"

#include "js/TypeDecls.h"

struct JSStructuredCloneReader;
struct JSStructuredCloneWriter;

struct JSPrincipals {
  /* Don't call "destroy"; use reference counting macros below. */
  mozilla::Atomic<int32_t, mozilla::SequentiallyConsistent> refcount{0};

#ifdef JS_DEBUG
  /* A helper to facilitate principals debugging. */
  uint32_t debugToken = 0;
#endif

  JSPrincipals() = default;

  struct RefCount {
    const int32_t value;
    constexpr explicit RefCount(int32_t value) : value(value) {}
    RefCount(const RefCount&) = delete;
  };
  /* Initialize a JSPrincipals with the given refcount in a constexpr-compatible
   * way. */
  explicit constexpr JSPrincipals(RefCount c) : refcount{c.value} {}

  void setDebugToken(int32_t token) {
#ifdef JS_DEBUG
    debugToken = token;
#endif
  }

  /*
   * Write the principals with the given |writer|. Return false on failure,
   * true on success.
   */
  virtual bool write(JSContext* cx, JSStructuredCloneWriter* writer) = 0;

  /*
   * Whether the principal corresponds to a System or AddOn Principal.
   * Technically this also checks for an ExpandedAddonPrincipal.
   */
  virtual bool isSystemOrAddonPrincipal() = 0;

  /*
   * This is not defined by the JS engine but should be provided by the
   * embedding.
   */
  JS_PUBLIC_API void dump();
};

extern JS_PUBLIC_API void JS_HoldPrincipals(JSPrincipals* principals);

extern JS_PUBLIC_API void JS_DropPrincipals(JSContext* cx,
                                            JSPrincipals* principals);

// Return whether the first principal subsumes the second. The exact meaning of
// 'subsumes' is left up to the browser. Subsumption is checked inside the JS
// engine when determining, e.g., which stack frames to display in a backtrace.
typedef bool (*JSSubsumesOp)(JSPrincipals* first, JSPrincipals* second);

namespace JS {
enum class RuntimeCode { JS, WASM };
enum class CompilationType { DirectEval, IndirectEval, Function, Undefined };
}  // namespace JS

/*
 * Used to check if a CSP instance wants to disable eval() and friends.
 * See JSContext::isRuntimeCodeGenEnabled() in vm/JSContext.cpp.
 *
 * codeString, compilationType, parameterStrings, bodyString, parameterArgs,
 * and bodyArg are defined in the "Dynamic Code Brand Checks" spec
 * (see https://tc39.es/proposal-dynamic-code-brand-checks).
 *
 * An Undefined compilationType is used for cases that are not covered by that
 * spec and unused parameters are null/empty. Currently, this includes Wasm
 * (only check if compilation is enabled) and ShadowRealmEval (only check
 * codeString).
 *
 * `outCanCompileStrings` is set to false if this callback prevents the
 * execution/compilation of the code and to true otherwise.
 *
 * Return false on failure, true on success. The |outCanCompileStrings|
 * parameter should not be modified in case of failure.
 */
typedef bool (*JSCSPEvalChecker)(
    JSContext* cx, JS::RuntimeCode kind, JS::Handle<JSString*> codeString,
    JS::CompilationType compilationType,
    JS::Handle<JS::StackGCVector<JSString*>> parameterStrings,
    JS::Handle<JSString*> bodyString,
    JS::Handle<JS::StackGCVector<JS::Value>> parameterArgs,
    JS::Handle<JS::Value> bodyArg, bool* outCanCompileStrings);

/*
 * Provide a string of code from an Object argument, to be used by eval.
 * See JSContext::getCodeForEval() in vm/JSContext.cpp as well as
 * https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostgetcodeforeval
 *
 * `code` is the JavaScript object passed by the user.
 * `outCode` is the JavaScript string to be actually executed, with nullptr
 *  meaning NO-CODE.
 *
 * Return false on failure, true on success. The |outCode| parameter should not
 * be modified in case of failure.
 */
typedef bool (*JSCodeForEvalOp)(JSContext* cx, JS::HandleObject code,
                                JS::MutableHandle<JSString*> outCode);

struct JSSecurityCallbacks {
  JSCSPEvalChecker contentSecurityPolicyAllows;
  JSCodeForEvalOp codeForEvalGets;
  JSSubsumesOp subsumes;
};

extern JS_PUBLIC_API void JS_SetSecurityCallbacks(
    JSContext* cx, const JSSecurityCallbacks* callbacks);

extern JS_PUBLIC_API const JSSecurityCallbacks* JS_GetSecurityCallbacks(
    JSContext* cx);

/*
 * Code running with "trusted" principals will be given a deeper stack
 * allocation than ordinary scripts. This allows trusted script to run after
 * untrusted script has exhausted the stack. This function sets the
 * runtime-wide trusted principal.
 *
 * This principals is not held (via JS_HoldPrincipals/JS_DropPrincipals).
 * Instead, the caller must ensure that the given principals stays valid for as
 * long as 'cx' may point to it. If the principals would be destroyed before
 * 'cx', JS_SetTrustedPrincipals must be called again, passing nullptr for
 * 'prin'.
 */
extern JS_PUBLIC_API void JS_SetTrustedPrincipals(JSContext* cx,
                                                  JSPrincipals* prin);

typedef void (*JSDestroyPrincipalsOp)(JSPrincipals* principals);

/*
 * Initialize the callback that is called to destroy JSPrincipals instance
 * when its reference counter drops to zero. The initialization can be done
 * only once per JS runtime.
 */
extern JS_PUBLIC_API void JS_InitDestroyPrincipalsCallback(
    JSContext* cx, JSDestroyPrincipalsOp destroyPrincipals);

/*
 * Read a JSPrincipals instance from the given |reader| and initialize the out
 * paratemer |outPrincipals| to the JSPrincipals instance read.
 *
 * Return false on failure, true on success. The |outPrincipals| parameter
 * should not be modified if false is returned.
 *
 * The caller is not responsible for calling JS_HoldPrincipals on the resulting
 * JSPrincipals instance, the JSReadPrincipalsOp must increment the refcount of
 * the resulting JSPrincipals on behalf of the caller.
 */
using JSReadPrincipalsOp = bool (*)(JSContext* cx,
                                    JSStructuredCloneReader* reader,
                                    JSPrincipals** outPrincipals);

/*
 * Initialize the callback that is called to read JSPrincipals instances from a
 * buffer. The initialization can be done only once per JS runtime.
 */
extern JS_PUBLIC_API void JS_InitReadPrincipalsCallback(
    JSContext* cx, JSReadPrincipalsOp read);

namespace JS {

class MOZ_RAII AutoHoldPrincipals {
  JSContext* cx_;
  JSPrincipals* principals_ = nullptr;

 public:
  explicit AutoHoldPrincipals(JSContext* cx, JSPrincipals* principals = nullptr)
      : cx_(cx) {
    reset(principals);
  }

  ~AutoHoldPrincipals() { reset(nullptr); }

  void reset(JSPrincipals* principals) {
    if (principals) {
      JS_HoldPrincipals(principals);
    }
    if (principals_) {
      JS_DropPrincipals(cx_, principals_);
    }
    principals_ = principals;
  }

  JSPrincipals* get() const { return principals_; }
};

}  // namespace JS

#endif /* js_Principals_h */

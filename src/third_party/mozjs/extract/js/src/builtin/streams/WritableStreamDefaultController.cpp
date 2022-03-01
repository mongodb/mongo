/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class WritableStreamDefaultController. */

#include "builtin/streams/WritableStreamDefaultController.h"

#include "builtin/streams/ClassSpecMacro.h"  // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/WritableStream.h"  // js::WritableStream
#include "builtin/streams/WritableStreamDefaultControllerOperations.h"  // js::WritableStreamDefaultControllerError
#include "js/CallArgs.h"              // JS::CallArgs{,FromVp}
#include "js/Class.h"                 // js::ClassSpec, JS_NULL_CLASS_OPS
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"  // JS{Function,Property}Spec, JS_{FS,PS}_END
#include "js/Value.h"         // JS::Value

#include "vm/Compartment-inl.h"  // js::UnwrapAndTypeCheckThis

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Rooted;
using JS::Value;

using js::ClassSpec;
using js::UnwrapAndTypeCheckThis;
using js::WritableStreamDefaultController;
using js::WritableStreamDefaultControllerError;

/*** 4.7. Class WritableStreamDefaultController *****************************/

/**
 * Streams spec, 4.7.3.
 * new WritableStreamDefaultController()
 */
bool WritableStreamDefaultController::constructor(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Step 1: Throw a TypeError.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BOGUS_CONSTRUCTOR,
                            "WritableStreamDefaultController");
  return false;
}

/**
 * Streams spec, 4.7.4.1. error(e)
 */
static bool WritableStreamDefaultController_error(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Step 1: If ! IsWritableStreamDefaultController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<WritableStreamDefaultController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<WritableStreamDefaultController>(cx, args,
                                                                  "error"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: Let state be this.[[controlledWritableStream]].[[state]].
  // Step 3: If state is not "writable", return.
  if (unwrappedController->stream()->writable()) {
    // Step 4: Perform ! WritableStreamDefaultControllerError(this, e).
    if (!WritableStreamDefaultControllerError(cx, unwrappedController,
                                              args.get(0))) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

static const JSPropertySpec WritableStreamDefaultController_properties[] = {
    JS_PS_END};

static const JSFunctionSpec WritableStreamDefaultController_methods[] = {
    JS_FN("error", WritableStreamDefaultController_error, 1, 0), JS_FS_END};

JS_STREAMS_CLASS_SPEC(WritableStreamDefaultController, 0, SlotCount,
                      ClassSpec::DontDefineConstructor, 0, JS_NULL_CLASS_OPS);

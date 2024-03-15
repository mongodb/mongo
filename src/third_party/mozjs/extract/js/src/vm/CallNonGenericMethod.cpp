/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/CallNonGenericMethod.h"

#include "proxy/Proxy.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/ProxyObject.h"
#include "vm/SelfHosting.h"

using namespace js;

bool JS::detail::CallMethodIfWrapped(JSContext* cx, IsAcceptableThis test,
                                     NativeImpl impl, const CallArgs& args) {
  HandleValue thisv = args.thisv();
  MOZ_ASSERT(!test(thisv));

  if (thisv.isObject()) {
    JSObject& thisObj = args.thisv().toObject();
    if (thisObj.is<ProxyObject>()) {
      return Proxy::nativeCall(cx, test, impl, args);
    }
  }

  if (IsCallSelfHostedNonGenericMethod(impl)) {
    return ReportIncompatibleSelfHostedMethod(cx, thisv);
  }

  ReportIncompatible(cx, args);
  return false;
}

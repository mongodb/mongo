/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_AsyncDisposableStackObject_h
#define builtin_AsyncDisposableStackObject_h

#include "builtin/DisposableStackObjectBase.h"
#include "vm/JSObject.h"

namespace js {

class AsyncDisposableStackObject : public DisposableStackObjectBase {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;

  static AsyncDisposableStackObject* create(
      JSContext* cx, JS::Handle<JSObject*> proto,
      JS::Handle<JS::Value> initialDisposeCapability =
          JS::UndefinedHandleValue);

 private:
  static const ClassSpec classSpec_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];

  static bool is(JS::Handle<JS::Value> val);

  static bool construct(JSContext* cx, unsigned argc, JS::Value* vp);
  static bool use_impl(JSContext* cx, const JS::CallArgs& args);
  static bool use(JSContext* cx, unsigned argc, JS::Value* vp);
  static bool disposed_impl(JSContext* cx, const JS::CallArgs& args);
  static bool disposed(JSContext* cx, unsigned argc, JS::Value* vp);
  static bool move_impl(JSContext* cx, const JS::CallArgs& args);
  static bool move(JSContext* cx, unsigned argc, JS::Value* vp);
  static bool defer_impl(JSContext* cx, const JS::CallArgs& args);
  static bool defer(JSContext* cx, unsigned argc, JS::Value* vp);
  static bool adopt_impl(JSContext* cx, const JS::CallArgs& args);
  static bool adopt(JSContext* cx, unsigned argc, JS::Value* vp);
};

} /* namespace js */

#endif /* builtin_AsyncDisposableStackObject_h */

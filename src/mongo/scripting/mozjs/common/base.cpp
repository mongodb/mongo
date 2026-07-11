// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/base.h"

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/Id.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TracingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec* BaseInfo::freeFunctions = nullptr;
const JSFunctionSpec* BaseInfo::methods = nullptr;

const char* const BaseInfo::inheritFrom = nullptr;

void BaseInfo::addProperty(JSContext* cx,
                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::HandleValue v) {}
void BaseInfo::call(JSContext* cx, JS::CallArgs args) {}
void BaseInfo::construct(JSContext* cx, JS::CallArgs args) {}
void BaseInfo::delProperty(JSContext* cx,
                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::ObjectOpResult& result) {}
void BaseInfo::enumerate(JSContext* cx,
                         JS::HandleObject obj,
                         JS::MutableHandleIdVector properties,
                         bool enumerableOnly) {}
void BaseInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {}
void BaseInfo::getProperty(JSContext* cx,
                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::HandleValue receiver,
                           JS::MutableHandleValue vp) {}
bool BaseInfo::mayResolve(const JSAtomState& names, jsid id, JSObject* maybeObj) {
    return false;
}
void BaseInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {}
void BaseInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {}
void BaseInfo::setProperty(JSContext* cx,
                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::HandleValue v,
                           JS::HandleValue receiver,
                           JS::ObjectOpResult& result) {}

void BaseInfo::trace(JSTracer* trc, JSObject* obj) {}

}  // namespace mozjs
}  // namespace mongo

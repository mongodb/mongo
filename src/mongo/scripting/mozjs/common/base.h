// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <jsapi.h>

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/Id.h>
#include <js/PropertySpec.h>
#include <js/TracingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * InstallType represents how we want this type overlayed in the JS world.
 *
 * Global is for regular types that get installed into the global scope
 * Private gives us the type, but doesn't make the prototype publicly available
 * OverNative is used to attach functionality to prototypes that are already there.
 */
enum class InstallType : char {
    Global = 0,
    Private,
    OverNative,
};

/**
 * The Base object for all info types
 *
 * It's difficult to access the array types correctly in a non constexpr world,
 * so we just stash some nullptrs that are universally available.
 */
struct BaseInfo {
    static const char* const inheritFrom;
    static const InstallType installType = InstallType::Global;
    static const JSFunctionSpec* freeFunctions;
    static const JSFunctionSpec* methods;
    static const unsigned classFlags = 0;
    static constexpr uint32_t finalizeFlag = JSCLASS_FOREGROUND_FINALIZE;
    static void addProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue v);
    static void call(JSContext* cx, JS::CallArgs args);
    static void construct(JSContext* cx, JS::CallArgs args);
    static void delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::ObjectOpResult& result);
    static void enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::MutableHandleIdVector properties,
                          bool enumerableOnly);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);
    static void getProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue receiver,
                            JS::MutableHandleValue vp);

    static bool mayResolve(const JSAtomState& names, jsid id, JSObject* maybeObj);
    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue v,
                            JS::HandleValue receiver,
                            JS::ObjectOpResult& result);
    static void trace(JSTracer* trc, JSObject* obj);
};

}  // namespace mozjs
}  // namespace mongo

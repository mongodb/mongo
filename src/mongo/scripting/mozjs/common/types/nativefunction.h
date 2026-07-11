// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/native_function.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wrapper for JS Interpreter agnostic functions. Think mapReduce, or any use
 * case that can tolerate automatic json <-> bson translation.
 *
 * The business end of the shim methods comes via ::call(). These types are
 * invokable as js functions, with a little bit of automatic translation for
 * arguments.
 *
 * This inherits from the global Function type.
 *
 * Also note that installType is private. So you can only get NativeFunctions
 * in JS via ::make() from C++.
 */
struct NativeFunctionInfo : public BaseInfo {
    enum Slots { NativeHolderSlot, NativeFunctionInfoSlotCount };

    static void call(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    static const char* const inheritFrom;
    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(NativeFunctionInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(toString);
    };

    static const JSFunctionSpec methods[2];

    static void make(JSContext* cx,
                     JS::MutableHandleObject obj,
                     NativeFunction function,
                     void* data);
};

}  // namespace mozjs
}  // namespace mongo

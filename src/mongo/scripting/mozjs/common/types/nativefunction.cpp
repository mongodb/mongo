// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/nativefunction.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/freeOpToJSContext.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <js/Array.h>
#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/ValueArray.h>

namespace mongo {
namespace mozjs {

const char* const NativeFunctionInfo::inheritFrom = "Function";
const char* const NativeFunctionInfo::className = "NativeFunction";

const JSFunctionSpec NativeFunctionInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NativeFunctionInfo),
    JS_FS_END,
};

namespace {

/**
 * Holder for the caller of ::make()'s callback function and context pointer
 */
class NativeHolder {
public:
    NativeHolder(NativeFunction func, void* ctx) : _func(func), _ctx(ctx) {}

    NativeFunction _func;
    void* _ctx;
};

NativeHolder* getHolder(JS::CallArgs args) {
    return JS::GetMaybePtrFromReservedSlot<NativeHolder>(&args.callee(),
                                                         NativeFunctionInfo::NativeHolderSlot);
}

}  // namespace

void NativeFunctionInfo::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);

    if (!holder) {
        // Calling the prototype
        args.rval().setUndefined();
        return;
    }

    JS::RootedObject robj(cx, JS::NewArrayObject(cx, args));
    if (!robj) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS::NewArrayObject");
    }

    BSONObj out = holder->_func(ObjectWrapper(cx, robj).toBSON(), holder->_ctx);

    ValueReader(cx, args.rval()).fromBSONElement(out.firstElement(), out, false);
}

void NativeFunctionInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto holder = JS::GetMaybePtrFromReservedSlot<NativeHolder>(obj, NativeHolderSlot);

    if (holder)
        trackedDelete(getCommonRuntime(freeOpToJSContext(gcCtx)), holder);
}

void NativeFunctionInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    str::stream ss;
    ss << "[native code]";

    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}


void NativeFunctionInfo::make(JSContext* cx,
                              JS::MutableHandleObject obj,
                              NativeFunction function,
                              void* data) {
    auto runtime = getCommonRuntime(cx);
    getProto<NativeFunctionInfo>(runtime).newObject(obj);
    JS::SetReservedSlot(
        obj, NativeHolderSlot, JS::PrivateValue(trackedNew<NativeHolder>(runtime, function, data)));
}

}  // namespace mozjs
}  // namespace mongo

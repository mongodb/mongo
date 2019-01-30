
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/nativefunction.h"

#include <cstdio>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const char* const NativeFunctionInfo::inheritFrom = "Function";
const char* const NativeFunctionInfo::className = "NativeFunction";

const JSFunctionSpec NativeFunctionInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NativeFunctionInfo), JS_FS_END,
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
    return static_cast<NativeHolder*>(JS_GetPrivate(&args.callee()));
}

}  // namespace

void NativeFunctionInfo::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);

    if (!holder) {
        // Calling the prototype
        args.rval().setUndefined();
        return;
    }

    JS::RootedObject robj(cx, JS_NewArrayObject(cx, args));
    if (!robj) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_NewArrayObject");
    }

    BSONObj out = holder->_func(ObjectWrapper(cx, robj).toBSON(), holder->_ctx);

    ValueReader(cx, args.rval()).fromBSONElement(out.firstElement(), out, false);
}

void NativeFunctionInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto holder = static_cast<NativeHolder*>(JS_GetPrivate(obj));

    if (holder)
        getScope(fop)->trackedDelete(holder);
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
    auto scope = getScope(cx);

    scope->getProto<NativeFunctionInfo>().newObject(obj);

    JS_SetPrivate(obj, scope->trackedNew<NativeHolder>(function, data));
}

}  // namespace mozjs
}  // namespace mongo

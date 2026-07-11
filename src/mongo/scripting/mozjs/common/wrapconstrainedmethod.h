// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <jsapi.h>

namespace mongo {
namespace mozjs {
namespace smUtils {

/**
 * Returns true if "value" is an instance of any of the types passed as
 * template parameters. Additionally sets isProto if the value is also the
 * prototype for that type.
 *
 * We recurse until we hit the void specialization which we set up by adding
 * void as the last type in wrapConstrainedMethod.
 */
template <typename T, typename... Args>
bool instanceOf(MozJSCommonRuntimeInterface* runtime, bool* isProto, JS::HandleValue value) {
    auto& proto = getProto<T>(runtime);

    if (proto.instanceOf(value)) {
        if (value.toObjectOrNull() == proto.getProto()) {
            *isProto = true;
        }

        return true;
    }

    return instanceOf<Args...>(runtime, isProto, value);
}

/**
 * Terminating specialization for instanceOf.
 *
 * We use this to identify the end of the template list in the general case.
 */
template <>
inline bool instanceOf<void>(MozJSCommonRuntimeInterface* runtime,
                             bool* isProto,
                             JS::HandleValue value) {
    return false;
}


/**
 * Wraps a method with an additional check against a list of possible wrap types.
 *
 * Template Parameters:
 *   T - A type with
 *       ::call - a static function of type void (JSContext* cx, JS::CallArgs args)
 *       ::name - a static function which returns a const char* with the type name
 *   noProto - whether the method can be invoked on the prototype
 *   Args - The list of types to check against runtime->getProto<T>().instanceOf
 *          for the thisv the method has been invoked against
 */
template <typename T, bool noProto, typename... Args>
bool wrapConstrainedMethod(JSContext* cx, unsigned argc, JS::Value* vp) {
    try {
        JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
        bool isProto = false;

        if (!args.thisv().isObject()) {
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << "Cannot call \"" << T::name() << "\" on non-object of type \""
                          << ValueWriter(cx, args.thisv()).typeAsString() << "\"");
        }

        if (!instanceOf<Args..., void>(getCommonRuntime(cx), &isProto, args.thisv())) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Cannot call \"" << T::name() << "\" on object of type \""
                                    << ObjectWrapper(cx, args.thisv()).getClassName() << "\"");
        }

        if (noProto && isProto) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Cannot call \"" << T::name() << "\" on prototype of \""
                                    << ObjectWrapper(cx, args.thisv()).getClassName() << "\"");
        }

        T::call(cx, args);
        return true;
    } catch (...) {
        mongoToJSException(cx);
        return false;
    }
}

}  // namespace smUtils
}  // namespace mozjs
}  // namespace mongo

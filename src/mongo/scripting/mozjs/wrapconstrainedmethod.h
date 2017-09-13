/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <jsapi.h>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stringutils.h"

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
bool instanceOf(MozJSImplScope* scope, bool* isProto, JS::HandleValue value) {
    auto& proto = scope->getProto<T>();

    if (proto.instanceOf(value)) {
        if (value.toObjectOrNull() == proto.getProto()) {
            *isProto = true;
        }

        return true;
    }

    return instanceOf<Args...>(scope, isProto, value);
}

/**
 * Terminating specialization for instanceOf.
 *
 * We use this to identify the end of the template list in the general case.
 */
template <>
inline bool instanceOf<void>(MozJSImplScope* scope, bool* isProto, JS::HandleValue value) {
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
 *   Args - The list of types to check against scope->getProto<T>().instanceOf
 *          for the thisv the method has been invoked against
 */
template <typename T, bool noProto, typename... Args>
bool wrapConstrainedMethod(JSContext* cx, unsigned argc, JS::Value* vp) {
    try {
        JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
        bool isProto = false;

        if (!args.thisv().isObject()) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Cannot call \"" << T::name()
                                    << "\" on non-object of type \""
                                    << ValueWriter(cx, args.thisv()).typeAsString()
                                    << "\"");
        }

        if (!instanceOf<Args..., void>(getScope(cx), &isProto, args.thisv())) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Cannot call \"" << T::name() << "\" on object of type \""
                                    << ObjectWrapper(cx, args.thisv()).getClassName()
                                    << "\"");
        }

        if (noProto && isProto) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Cannot call \"" << T::name() << "\" on prototype of \""
                                    << ObjectWrapper(cx, args.thisv()).getClassName()
                                    << "\"");
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

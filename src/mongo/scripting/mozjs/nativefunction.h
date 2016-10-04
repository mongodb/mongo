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

#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wraptype.h"

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
    static void call(JSContext* cx, JS::CallArgs args);
    static void finalize(JSFreeOp* fop, JSObject* obj);

    static const char* const inheritFrom;
    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
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

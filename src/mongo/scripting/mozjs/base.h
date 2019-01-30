
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

#pragma once

#include <jsapi.h>

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
                          JS::AutoIdVector& properties,
                          bool enumerableOnly);
    static void finalize(js::FreeOp* fop, JSObject* obj);
    static void getProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue receiver,
                            JS::MutableHandleValue vp);
    static void hasInstance(JSContext* cx,
                            JS::HandleObject obj,
                            JS::MutableHandleValue vp,
                            bool* bp);
    static bool mayResolve(const JSAtomState& names, jsid id, JSObject* maybeObj);
    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue v,
                            JS::HandleValue receiver,
                            JS::ObjectOpResult& result);
};

}  // namespace mozjs
}  // namespace mongo

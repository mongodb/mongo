
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

#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * The "MongoStatus" Javascript object.
 *
 * This type wraps the "Status" type in the server, allowing for lossless throwing of mongodb native
 * exceptions through javascript.  It can be created (albeit without sidecar) from javascript.
 * These are also created automatically when exceptions are thrown from native c++ functions.
 *
 * They are somewhat special, in that the prototype for each MongoStatus object is actually an Error
 * object specific to that status object.  This allows Error-like behavior such as useful stack
 * traces, and instanceOf Error.
 */
struct MongoStatusInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(js::FreeOp* fop, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(code);
        MONGO_DECLARE_JS_FUNCTION(reason);
        MONGO_DECLARE_JS_FUNCTION(stack);
    };

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static const char* const className;
    static const char* const inheritFrom;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;

    static Status toStatus(JSContext* cx, JS::HandleObject object);
    static Status toStatus(JSContext* cx, JS::HandleValue value);
    static void fromStatus(JSContext* cx, Status status, JS::MutableHandleValue value);
};

}  // namespace mozjs
}  // namespace mongo

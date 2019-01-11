
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

#include "mongo/client/dbclient_base.h"
#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * Wraps a Session in javascript
 *
 * Note that the install is private, so this class should only be constructible
 * from C++. Current callers are all via the Mongo object.
 */
struct SessionInfo : public BaseInfo {
    static void finalize(js::FreeOp* fop, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(end);
        MONGO_DECLARE_JS_FUNCTION(getId);
        MONGO_DECLARE_JS_FUNCTION(getTxnState);
        MONGO_DECLARE_JS_FUNCTION(setTxnState);
        MONGO_DECLARE_JS_FUNCTION(getTxnNumber);
        MONGO_DECLARE_JS_FUNCTION(setTxnNumber);
        MONGO_DECLARE_JS_FUNCTION(incrementTxnNumber);
    };

    static const JSFunctionSpec methods[8];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
    static const InstallType installType = InstallType::Private;

    static void make(JSContext* cx,
                     JS::MutableHandleObject obj,
                     std::shared_ptr<DBClientBase> client,
                     BSONObj lsid);
};

}  // namespace mozjs
}  // namespace mongo

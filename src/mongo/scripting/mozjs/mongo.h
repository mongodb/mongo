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

#include <memory>

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TracingAPI.h>
#include <js/TypeDecls.h>

#include "mongo/client/dbclient_base.h"
#include "mongo/scripting/mozjs/base.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

using EncryptedDBClientCallback = std::shared_ptr<DBClientBase>(std::shared_ptr<DBClientBase>,
                                                                JS::HandleValue,
                                                                JS::HandleObject,
                                                                JSContext*);

using EncryptedDBClientFromExistingCallback = std::shared_ptr<DBClientBase>(
    std::shared_ptr<DBClientBase>, std::shared_ptr<DBClientBase>, JSContext*);

using GetNestedConnectionCallback = DBClientBase*(DBClientBase*);

void setEncryptedDBClientCallbacks(EncryptedDBClientCallback* encCallback,
                                   EncryptedDBClientFromExistingCallback* encFromExistingCallback,
                                   GetNestedConnectionCallback* getCallback);

/**
 * Shared code for the "Mongo" javascript object.
 *
 * The idea here is that there is a lot of shared functionality between the
 * "Mongo" we see in the shell and the "Mongo" in dbeval.  So we provide one
 * info type with common code and differentiate with varying constructors.
 */
struct MongoBase : public BaseInfo {
    static void finalize(JSFreeOp* fop, JSObject* obj);
    static void trace(JSTracer* trc, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(auth);
        MONGO_DECLARE_JS_FUNCTION(close);
        MONGO_DECLARE_JS_FUNCTION(cleanup);
        MONGO_DECLARE_JS_FUNCTION(compact);

        MONGO_DECLARE_JS_FUNCTION(setAutoEncryption);
        MONGO_DECLARE_JS_FUNCTION(getAutoEncryptionOptions);
        MONGO_DECLARE_JS_FUNCTION(unsetAutoEncryption);
        MONGO_DECLARE_JS_FUNCTION(toggleAutoEncryption);
        MONGO_DECLARE_JS_FUNCTION(isAutoEncryptionEnabled);
        MONGO_DECLARE_JS_FUNCTION(cursorHandleFromId);
        MONGO_DECLARE_JS_FUNCTION(find);
        MONGO_DECLARE_JS_FUNCTION(generateDataKey);
        MONGO_DECLARE_JS_FUNCTION(getDataKeyCollection);
        MONGO_DECLARE_JS_FUNCTION(encrypt);
        MONGO_DECLARE_JS_FUNCTION(decrypt);
        MONGO_DECLARE_JS_FUNCTION(insert);
        MONGO_DECLARE_JS_FUNCTION(isReplicaSetConnection);
        MONGO_DECLARE_JS_FUNCTION(_markNodeAsFailed);
        MONGO_DECLARE_JS_FUNCTION(logout);
        MONGO_DECLARE_JS_FUNCTION(remove);
        MONGO_DECLARE_JS_FUNCTION(update);
        MONGO_DECLARE_JS_FUNCTION(getMinWireVersion);
        MONGO_DECLARE_JS_FUNCTION(getMaxWireVersion);
        MONGO_DECLARE_JS_FUNCTION(isReplicaSetMember);
        MONGO_DECLARE_JS_FUNCTION(isMongos);
        MONGO_DECLARE_JS_FUNCTION(isTLS);
        MONGO_DECLARE_JS_FUNCTION(isGRPC);
        MONGO_DECLARE_JS_FUNCTION(getApiParameters);
        MONGO_DECLARE_JS_FUNCTION(_getCompactionTokens);
        MONGO_DECLARE_JS_FUNCTION(_runCommandImpl);
        MONGO_DECLARE_JS_FUNCTION(_startSession);
        MONGO_DECLARE_JS_FUNCTION(_setOIDCIdPAuthCallback);
        MONGO_DECLARE_JS_FUNCTION(_refreshAccessToken);
    };

    static const JSFunctionSpec methods[31];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
};

/**
 * The shell variant of "Mongo"
 */
struct MongoExternalInfo : public MongoBase {
    static void construct(JSContext* cx, JS::CallArgs args);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(_forgetReplSet);
        MONGO_DECLARE_JS_FUNCTION(load);
        MONGO_DECLARE_JS_FUNCTION(quit);
    };

    static const JSFunctionSpec freeFunctions[4];
};

class EncryptionCallbacks {
public:
    virtual void generateDataKey(JSContext* cx, JS::CallArgs args) = 0;
    virtual void getDataKeyCollection(JSContext* cx, JS::CallArgs args) = 0;
    virtual void encrypt(MozJSImplScope* scope, JSContext* cx, JS::CallArgs args) = 0;
    virtual void decrypt(MozJSImplScope* scope, JSContext* cx, JS::CallArgs args) = 0;
    virtual void cleanup(JSContext* cx, JS::CallArgs args) = 0;
    virtual void compact(JSContext* cx, JS::CallArgs args) = 0;
    virtual void trace(JSTracer* trc) = 0;
    virtual void getEncryptionOptions(JSContext* cx, JS::CallArgs args) = 0;
    virtual void _getCompactionTokens(JSContext* cx, JS::CallArgs args) = 0;
};

void setEncryptionCallbacks(DBClientBase* conn, EncryptionCallbacks* callbacks);

}  // namespace mozjs
}  // namespace mongo

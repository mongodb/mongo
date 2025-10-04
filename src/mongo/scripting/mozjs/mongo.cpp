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

#include "mongo/scripting/mozjs/mongo.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/client_transient_tls_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/mongo_shell_options_gen.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/client/sasl_oidc_client_conversation.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/metadata.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/cursor.h"
#include "mongo/scripting/mozjs/cursor_handle.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/numberlong.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/session.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertyDescriptor.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TracingAPI.h>
#include <js/TypeDecls.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace mozjs {

const JSFunctionSpec MongoBase::methods[] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(auth, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(cleanup, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(close, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(compact, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(setAutoEncryption, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getAutoEncryptionOptions, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(unsetAutoEncryption, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toggleAutoEncryption, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isAutoEncryptionEnabled, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(cursorHandleFromId, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(find, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(generateDataKey, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getDataKeyCollection, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(encrypt, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(decrypt, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isReplicaSetConnection, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_markNodeAsFailed, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(logout, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getMinWireVersion, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getMaxWireVersion, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isReplicaSetMember, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isMongos, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isTLS, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isGRPC, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getApiParameters, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_getCompactionTokens, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_runCommandImpl, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_startSession, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_setOIDCIdPAuthCallback, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_refreshAccessToken, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getShellPort, MongoExternalInfo),
    JS_FS_END,
};

const char* const MongoBase::className = "Mongo";

EncryptedDBClientCallback* encryptedDBClientCallback = nullptr;
EncryptedDBClientFromExistingCallback* encryptedDBClientFromExistingCallback = nullptr;
GetNestedConnectionCallback* getNestedConnectionCallback = nullptr;

const JSFunctionSpec MongoExternalInfo::freeFunctions[4] = {
    MONGO_ATTACH_JS_FUNCTION(_forgetReplSet),
    MONGO_ATTACH_JS_FUNCTION(load),
    MONGO_ATTACH_JS_FUNCTION(quit),
    JS_FS_END,
};

namespace {

// This class holds a "raw" DBClient connection, along with an optional "encrypted" DBClient
// that performs automatic encryption of requests before forwarding them to the raw connection.
// If auto encryption is enabled, this returns the encrypted DBClient in getConnection().
class DBClientWithAutoEncryption {
public:
    DBClientWithAutoEncryption(std::shared_ptr<DBClientBase> rawConn,
                               std::shared_ptr<DBClientBase> encryptedConn)
        : _rawConn(std::move(rawConn)),
          _encryptedConn(std::move(encryptedConn)),
          _encryptionEnabled(_encryptedConn != nullptr) {
        invariant(_rawConn);
    }

    void setEncryptedConnection(std::shared_ptr<DBClientBase> encryptedConn) {
        _encryptedConn = std::move(encryptedConn);
    }

    std::shared_ptr<DBClientBase>& getConnection() {
        return this->isEncryptionEnabled() ? _encryptedConn : _rawConn;
    }

    std::shared_ptr<DBClientBase>& getRawConnection() {
        return _rawConn;
    }

    std::shared_ptr<DBClientBase>& getEncryptedConnection() {
        return _encryptedConn;
    }

    bool isEncryptionEnabled() const {
        return _encryptionEnabled && _encryptedConn != nullptr;
    }

    void toggleAutoEncryption(bool enable) {
        uassert(7760001,
                "Auto encryption is not configured on this connection",
                _encryptedConn != nullptr);
        _encryptionEnabled = enable;
    }

private:
    std::shared_ptr<DBClientBase> _rawConn;
    std::shared_ptr<DBClientBase> _encryptedConn;
    bool _encryptionEnabled;
};

const std::shared_ptr<DBClientWithAutoEncryption>& getDBClientWithAutoEncryptionRef(
    JS::CallArgs& args) {
    auto ret = JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
        args.thisv().toObjectOrNull(), MongoBase::DBClientWithAutoEncryptionSlot);
    uassert(
        ErrorCodes::BadValue, "Trying to get connection for closed Mongo object", *ret != nullptr);
    return *ret;
}

const std::shared_ptr<DBClientBase>& getConnectionRef(JS::CallArgs& args) {
    return getDBClientWithAutoEncryptionRef(args)->getConnection();
}

DBClientBase* getConnection(JS::CallArgs& args) {
    return getConnectionRef(args).get();
}

bool isUnacknowledged(const BSONObj& cmdObj) {
    auto wc = cmdObj["writeConcern"];
    return wc && wc["w"].isNumber() && wc["w"].safeNumberLong() == 0;
}

void returnOk(JSContext* cx, JS::CallArgs& args) {
    ValueReader(cx, args.rval()).fromBSON(BSON("ok" << 1), nullptr, false);
}

void setCursor(MozJSImplScope* scope,
               JS::HandleObject target,
               std::unique_ptr<DBClientCursor> cursor,
               JS::CallArgs& args) {
    auto client = getConnectionRef(args);

    // Copy the client shared pointer to up the refcount
    JS::SetReservedSlot(
        target,
        CursorInfo::CursorHolderSlot,
        JS::PrivateValue(scope->trackedNew<CursorInfo::CursorHolder>(std::move(cursor), client)));
}

void setCursorHandle(MozJSImplScope* scope,
                     JS::HandleObject target,
                     NamespaceString ns,
                     long long cursorId,
                     JS::CallArgs& args) {
    auto client = getConnectionRef(args);

    // Copy the client shared pointer to up the refcount.
    JS::SetReservedSlot(target,
                        CursorHandleInfo::CursorTrackerSlot,
                        JS::PrivateValue(scope->trackedNew<CursorHandleInfo::CursorTracker>(
                            std::move(ns), cursorId, client)));
}

void setHiddenMongo(JSContext* cx, JS::HandleValue value, JS::CallArgs& args) {
    ObjectWrapper o(cx, args.rval());
    if (!o.hasField(InternedString::_mongo)) {
        o.defineProperty(InternedString::_mongo, value, JSPROP_READONLY | JSPROP_PERMANENT);
    }
}

void setHiddenMongo(JSContext* cx, JS::CallArgs& args) {
    setHiddenMongo(cx, args.thisv(), args);
}

void setHiddenMongo(JSContext* cx,
                    std::shared_ptr<DBClientBase> resPtr,
                    std::shared_ptr<DBClientWithAutoEncryption> origClient,
                    JS::CallArgs& args) {
    ObjectWrapper o(cx, args.rval());
    // If the connection that ran the command is the same as conn, then we set a hidden "_mongo"
    // property on the returned object that is just "this" Mongo object.
    if (resPtr.get() == origClient->getConnection().get()) {
        setHiddenMongo(cx, args.thisv(), args);
    } else {
        JS::RootedObject newMongo(cx);

        auto scope = getScope(cx);
        scope->getProto<MongoExternalInfo>().newObject(&newMongo);

        auto host = resPtr->getServerAddress();

        auto encConn = origClient->getEncryptedConnection();
        if (encConn && encryptedDBClientFromExistingCallback) {
            encConn = encryptedDBClientFromExistingCallback(encConn, resPtr, cx);
        }

        auto newClient = std::make_unique<DBClientWithAutoEncryption>(std::move(resPtr), encConn);
        JS::SetReservedSlot(
            newMongo,
            MongoBase::DBClientWithAutoEncryptionSlot,
            JS::PrivateValue(scope->trackedNew<std::shared_ptr<DBClientWithAutoEncryption>>(
                newClient.release())));

        ObjectWrapper from(cx, args.thisv());
        ObjectWrapper to(cx, newMongo);
        for (const auto& k :
             {InternedString::slaveOk, InternedString::defaultDB, InternedString::authenticated}) {
            JS::RootedValue tmpValue(cx);
            from.getValue(k, &tmpValue);
            to.setValue(k, tmpValue);
        }

        // 'newMongo' is a direct connection to an individual server. Its "host" property therefore
        // reports the stringified HostAndPort of the underlying DBClientConnection.
        to.setString(InternedString::host, host);

        JS::RootedValue value(cx);
        value.setObjectOrNull(newMongo);
        setHiddenMongo(cx, value, args);
    }
}

EncryptionCallbacks* getEncryptionCallbacks(DBClientBase* conn) {
    auto callbackPtr = dynamic_cast<EncryptionCallbacks*>(conn);
    uassert(31083,
            "Field Level Encryption must be used in enterprise mode with the correct parameters",
            callbackPtr != nullptr);
    return callbackPtr;
}

std::shared_ptr<DBClientBase> runEncryptedDBClientCallback(std::shared_ptr<DBClientBase> conn,
                                                           JS::HandleValue arg,
                                                           JS::HandleObject mongoConnection,
                                                           JSContext* cx) {
    if (encryptedDBClientCallback != nullptr) {
        return encryptedDBClientCallback(std::move(conn), arg, mongoConnection, cx);
    }
    return nullptr;
}

}  // namespace

void MongoBase::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto conn = JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
        obj, DBClientWithAutoEncryptionSlot);

    if (conn) {
        getScope(gcCtx)->trackedDelete(conn);
    }
}

void MongoBase::trace(JSTracer* trc, JSObject* obj) {
    auto client = JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
        obj, DBClientWithAutoEncryptionSlot);
    if (!client || !(*client)) {
        return;
    }
    // Trace the encrypted connection if it has been allocated.
    auto encryptedConn = (*client)->getEncryptedConnection();

    auto callbackPtr = dynamic_cast<EncryptionCallbacks*>(encryptedConn.get());
    if (callbackPtr != nullptr) {
        callbackPtr->trace(trc);
    }
}

void MongoBase::Functions::close::call(JSContext* cx, JS::CallArgs args) {
    getConnection(args);

    auto thisv = args.thisv().toObjectOrNull();
    auto conn = JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
        thisv, DBClientWithAutoEncryptionSlot);

    conn->reset();

    args.rval().setUndefined();
}

namespace {

/**
 * Common implementation for:
 *   object Mongo._runCommandImpl(string dbname, object cmd, int options, object token)
 *
 * Extra is for connection-wide metadata to pass with any given runCommand.
 */
template <typename Params, typename MakeRequest>
void doRunCommand(JSContext* cx, JS::CallArgs args, MakeRequest makeRequest) {
    uassert(ErrorCodes::BadValue,
            str::stream() << Params::kCommandName << " needs 4 args",
            args.length() >= 4);
    uassert(ErrorCodes::BadValue,
            str::stream() << "The database parameter to " << Params::kCommandName
                          << " must be a string",
            args.get(0).isString());
    uassert(ErrorCodes::BadValue,
            str::stream() << "The " << Params::kArg1Name << " parameter to " << Params::kCommandName
                          << " must be an object",
            args.get(1).isObject());

    // Arg2 is specialization defined, see makeRequest().

    auto database = ValueWriter(cx, args.get(0)).toString();
    auto arg = ValueWriter(cx, args.get(1)).toBSON();

    auto request = makeRequest(database, arg);
    if (auto tokenArg = args.get(3); tokenArg.isString()) {
        if (auto token = ValueWriter(cx, tokenArg).toString(); !token.empty()) {
            request.validatedTenancyScope = auth::ValidatedTenancyScopeFactory::create(
                token, auth::ValidatedTenancyScopeFactory::InitForShellTag{});
        }
    } else {
        uassert(ErrorCodes::BadValue,
                fmt::format("The token parameter to {} must be a string", Params::kCommandName),
                tokenArg.isUndefined());
    }

    const auto& conn = getConnectionRef(args);
    if (isUnacknowledged(request.body)) {
        conn->runFireAndForgetCommand(request);
        setHiddenMongo(cx, args);
        returnOk(cx, args);
        return;
    }

    auto res = conn->runCommandWithTarget(request, conn);

    auto reply = std::get<0>(res)->getCommandReply();
    if constexpr (Params::kHoistReply) {
        constexpr auto kCommandReplyField = "commandReply"_sd;
        reply = BSON(kCommandReplyField << reply);
    } else {
        // The returned object is not read only as some of our tests depend on modifying it.
        // Make a copy here because we want a copy after we dump cmdRes
        reply = reply.getOwned();
    }

    ValueReader(cx, args.rval()).fromBSON(reply, nullptr, false /* read only */);

    auto origClient = JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
        args.thisv().toObjectOrNull(), MongoBase::DBClientWithAutoEncryptionSlot);
    setHiddenMongo(cx, std::get<1>(res), *origClient, args);

    ObjectWrapper o(cx, args.rval());
    if (!o.hasField(InternedString::_commandObj)) {
        o.defineProperty(
            InternedString::_commandObj, args.get(1), JSPROP_READONLY | JSPROP_PERMANENT);
    }
}

}  // namespace

struct RunCommandParams {
    static constexpr bool kHoistReply = false;
    static constexpr auto kCommandName = "runCommand"_sd;
    static constexpr auto kArg1Name = "cmdObj"_sd;
};

void MongoBase::Functions::_runCommandImpl::call(JSContext* cx, JS::CallArgs args) {
    doRunCommand<RunCommandParams>(cx, args, [&](StringData database, BSONObj cmd) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "The options parameter to runCommand must be a number",
                args.get(2).isNumber());
        auto options = ValueWriter(cx, args.get(2)).toInt32();
        return rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, database),
            cmd,
            options,
            mongo::auth::ValidatedTenancyScope::kNotRequired);
    });
}

void MongoBase::Functions::find::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    tassert(6887100, "wrong number of args for find operation", args.length() == 3);
    tassert(6887101, "first arg must be an object", args.get(0).isObject());
    tassert(6887102, "second arg must be an object", args.get(1).isObject());
    tassert(6887103, "third arg must be a boolean", args.get(2).isBoolean());

    auto conn = getConnection(args);

    const BSONObj cmdObj = ValueWriter(cx, args.get(0)).toBSON();
    const BSONObj readPreference = ValueWriter(cx, args.get(1)).toBSON();
    const bool isExhaust = ValueWriter(cx, args.get(2)).toBoolean();

    FindCommandRequest findCmdRequest =
        FindCommandRequest::parse(cmdObj, IDLParserContext("FindCommandRequest"));
    ReadPreferenceSetting readPref;
    if (!readPreference.isEmpty()) {
        readPref = uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPreference));
    }
    ExhaustMode exhaustMode = isExhaust ? ExhaustMode::kOn : ExhaustMode::kOff;

    std::unique_ptr<DBClientCursor> cursor =
        conn->find(std::move(findCmdRequest), readPref, exhaustMode);
    uassert(ErrorCodes::InternalError, "error doing query: failed", cursor);

    JS::RootedObject c(cx);
    scope->getProto<CursorInfo>().newObject(&c);

    setCursor(scope, c, std::move(cursor), args);

    args.rval().setObjectOrNull(c);
}

void MongoBase::Functions::auth::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    uassert(ErrorCodes::BadValue, "no connection", conn);
    uassert(ErrorCodes::BadValue, "mongoAuth takes exactly 1 object argument", args.length() == 1);

    conn->auth(ValueWriter(cx, args.get(0)).toBSON());
    args.rval().setBoolean(true);
}

void MongoBase::Functions::generateDataKey::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->generateDataKey(cx, args);
}

void MongoBase::Functions::getDataKeyCollection::call(JSContext* cx, JS::CallArgs args) {
    JS::RootedValue collection(cx);
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->getDataKeyCollection(cx, args);
}

void MongoBase::Functions::encrypt::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->encrypt(scope, cx, args);
}

void MongoBase::Functions::decrypt::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->decrypt(scope, cx, args);
}

void MongoBase::Functions::cleanup::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->cleanup(cx, args);
}

void MongoBase::Functions::setAutoEncryption::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() < 1)
        uasserted(ErrorCodes::BadValue, "setAutoEncryption needs at least 1 arg");

    std::shared_ptr<DBClientWithAutoEncryption> kvClient;
    JS::RootedObject keyvaultConn(cx);
    if (args.length() > 1 && !args.get(1).isUndefined()) {
        uassert(7760001,
                str::stream() << "the second parameter to setAutoEncryption() must be an object",
                args.get(1).isObject());
        keyvaultConn.set(args.get(1).toObjectOrNull());
        kvClient = *JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
            args.get(1).toObjectOrNull(), DBClientWithAutoEncryptionSlot);

    } else {
        keyvaultConn.set(args.thisv().toObjectOrNull());
        kvClient = *JS::GetMaybePtrFromReservedSlot<std::shared_ptr<DBClientWithAutoEncryption>>(
            args.thisv().toObjectOrNull(), DBClientWithAutoEncryptionSlot);
    }

    auto client = getDBClientWithAutoEncryptionRef(args);

    if (client->getEncryptedConnection() != nullptr) {
        uasserted(ErrorCodes::BadValue, "Auto encryption is already set on this connection");
    }

    auto encConn =
        runEncryptedDBClientCallback(client->getRawConnection(), args.get(0), keyvaultConn, cx);
    bool retval = encConn != nullptr;
    if (encConn) {
        client->setEncryptedConnection(std::move(encConn));
    }
    args.rval().setBoolean(retval);
}

void MongoBase::Functions::getAutoEncryptionOptions::call(JSContext* cx, JS::CallArgs args) {
    auto client = getDBClientWithAutoEncryptionRef(args);
    auto encConn = client->getEncryptedConnection();
    if (encConn) {
        auto ptr = getEncryptionCallbacks(encConn.get());
        ptr->getEncryptionOptions(cx, args);
    } else {
        // no auto-encryption is set, so return undefined
        args.rval().setUndefined();
    }
}

void MongoBase::Functions::_getCompactionTokens::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->_getCompactionTokens(cx, args);
}

void MongoBase::Functions::unsetAutoEncryption::call(JSContext* cx, JS::CallArgs args) {
    auto client = getDBClientWithAutoEncryptionRef(args);
    client->toggleAutoEncryption(false);
    client->setEncryptedConnection(nullptr);
    args.rval().setBoolean(true);
}

void MongoBase::Functions::toggleAutoEncryption::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "toggleAutoEncryption needs 1 arg");
    if (!args.get(0).isBoolean()) {
        uasserted(ErrorCodes::BadValue, "first argument to toggleAutoEncryption must be a boolean");
    }
    auto enable = ValueWriter(cx, args.get(0)).toBoolean();
    auto client = getDBClientWithAutoEncryptionRef(args);
    client->toggleAutoEncryption(enable);
    args.rval().setBoolean(true);
}

void MongoBase::Functions::isAutoEncryptionEnabled::call(JSContext* cx, JS::CallArgs args) {
    auto client = getDBClientWithAutoEncryptionRef(args);
    args.rval().setBoolean(client->isEncryptionEnabled());
}

void MongoBase::Functions::compact::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    auto ptr = getEncryptionCallbacks(conn);
    ptr->compact(cx, args);
}

void MongoBase::Functions::logout::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "logout needs 1 arg");

    JSStringWrapper jsstr;
    const DatabaseName dbName =
        DatabaseNameUtil::deserialize(boost::none,
                                      ValueWriter(cx, args.get(0)).toStringData(&jsstr),
                                      SerializationContext::stateDefault());

    BSONObj ret;
    auto conn = getConnection(args);
    if (conn) {
        conn->logout(dbName, ret);
    }

    // Make a copy because I want to insulate us from whether conn->logout
    // writes an owned bson or not
    ValueReader(cx, args.rval()).fromBSON(ret.getOwned(), nullptr, false);
}

void MongoBase::Functions::cursorHandleFromId::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 2) {
        uasserted(ErrorCodes::BadValue, "cursorHandleFromId needs 2 args");
    }

    if (!scope->getProto<NumberLongInfo>().instanceOf(args.get(1))) {
        uasserted(ErrorCodes::BadValue, "2nd arg must be a NumberLong");
    }

    // getConnectionRef verifies that the connection is still open
    getConnectionRef(args);

    std::string ns = ValueWriter(cx, args.get(0)).toString();
    long long cursorId = NumberLongInfo::ToNumberLong(cx, args.get(1));

    JS::RootedObject c(cx);
    scope->getProto<CursorHandleInfo>().newObject(&c);

    setCursorHandle(scope, c, NamespaceString::createNamespaceString_forTest(ns), cursorId, args);

    args.rval().setObjectOrNull(c);
}

void MongoBase::Functions::isReplicaSetConnection::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (args.length() != 0) {
        uasserted(ErrorCodes::BadValue, "isReplicaSetConnection takes no args");
    }

    args.rval().setBoolean(conn->type() == ConnectionString::ConnectionType::kReplicaSet);
}

void MongoBase::Functions::_markNodeAsFailed::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 3) {
        uasserted(ErrorCodes::BadValue, "_markNodeAsFailed needs 3 args");
    }

    if (!args.get(0).isString()) {
        uasserted(ErrorCodes::BadValue,
                  "first argument to _markNodeAsFailed must be a stringified host and port");
    }

    if (!args.get(1).isNumber()) {
        uasserted(ErrorCodes::BadValue,
                  "second argument to _markNodeAsFailed must be a numeric error code");
    }

    if (!args.get(2).isString()) {
        uasserted(ErrorCodes::BadValue,
                  "third argument to _markNodeAsFailed must be a stringified reason");
    }

    DBClientReplicaSet* rsConn = dynamic_cast<DBClientReplicaSet*>(getConnection(args));
    if (!rsConn && getNestedConnectionCallback) {
        DBClientBase* base = getNestedConnectionCallback(getConnection(args));
        if (base) {
            rsConn = dynamic_cast<DBClientReplicaSet*>(base);
        }
    }

    if (!rsConn) {
        uasserted(ErrorCodes::BadValue, "connection object is not a replica set object");
    }

    auto hostAndPort = ValueWriter(cx, args.get(0)).toString();
    auto code = ValueWriter(cx, args.get(1)).toInt32();
    auto reason = ValueWriter(cx, args.get(2)).toString();

    const auto& replicaSetName = rsConn->getSetName();
    ReplicaSetMonitor::get(replicaSetName)
        ->failedHost(HostAndPort(hostAndPort),
                     Status{static_cast<ErrorCodes::Error>(code), reason});

    args.rval().setUndefined();
}

void setEncryptedDBClientCallbacks(EncryptedDBClientCallback* encCallback,
                                   EncryptedDBClientFromExistingCallback* encFromExistingCallback,
                                   GetNestedConnectionCallback* getCallback) {
    encryptedDBClientCallback = encCallback;
    encryptedDBClientFromExistingCallback = encFromExistingCallback;
    getNestedConnectionCallback = getCallback;
}

/**
 * Implements new Mongo() in jstests with the following ordered parameters:
 *
 * @param {string} uri - The connection URI for the MongoDB instance.
 * @param {function} encryptedDBClientCallback - Callback function for handling the encrypted
 * database client.
 * @param {object} {api: {...}, tls: {...}}: - API options object containing version information or
 * TLS options object to create a new transient connection.
 */
void MongoExternalInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    std::string host("127.0.0.1");

    if (args.length() > 0 && args.get(0).isString()) {
        host = ValueWriter(cx, args.get(0)).toString();
    }

    auto cs = uassertStatusOK(MongoURI::parse(host));

    MongoShellOptions mongoShellOptions;
    if (args.length() > 2 && !args.get(2).isUndefined()) {
        uassert(4938000,
                str::stream() << "the 'options' parameter to Mongo() must be an object",
                args.get(2).isObject());
        mongoShellOptions = MongoShellOptions::parse(ValueWriter(cx, args.get(2)).toBSON());
    }

    ClientAPIVersionParameters apiParameters;
    if (auto optApiParameters = mongoShellOptions.getApi()) {
        apiParameters = *optApiParameters;
        if (apiParameters.getDeprecationErrors().value_or(false) ||
            apiParameters.getStrict().value_or(false)) {
            uassert(4938002,
                    "the 'api' option for Mongo() must include 'version' if it includes "
                    "'strict' or 'deprecationErrors'",
                    apiParameters.getVersion());
        }
    }

#ifdef MONGO_CONFIG_GRPC
    if (mongoShellOptions.getGRPC()) {
        cs.setIsGRPC(mongoShellOptions.getGRPC().get());
    }
#endif

    boost::optional<TransientSSLParams> transientSSLParams;
    if (mongoShellOptions.getTls()) {
        const auto& tlsOpts = mongoShellOptions.getTls();

        TLSCredentials tlsCredentials;
        tlsCredentials.tlsPEMKeyFile = std::string{tlsOpts->getCertificateKeyFile().value_or("")};
        tlsCredentials.tlsPEMKeyPassword =
            std::string{tlsOpts->getCertificateKeyFilePassword().value_or("")};
        tlsCredentials.tlsCAFile = std::string{tlsOpts->getCAFile().value_or("")};
        tlsCredentials.tlsCRLFile = std::string{tlsOpts->getCRLFile().value_or("")};
        tlsCredentials.tlsAllowInvalidHostnames = tlsOpts->getAllowInvalidHostnames();
        tlsCredentials.tlsAllowInvalidCertificates = tlsOpts->getAllowInvalidCertificates();

#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
        if (tlsOpts->getCertificateSelector()) {
            uassertStatusOK(
                parseCertificateSelector(&tlsCredentials.tlsCertificateSelector,
                                         "tls.certificateSelector",
                                         std::string{tlsOpts->getCertificateSelector().value()}));
        }
#endif  // MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS

        transientSSLParams = TransientSSLParams(tlsCredentials);
    }

#ifdef MONGO_CONFIG_GRPC
    if (cs.isGRPC()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot enable gRPC mode when connecting to a replica set",
                cs.type() != ConnectionString::ConnectionType::kReplicaSet);

        uassert(ErrorCodes::InvalidOptions,
                "Authentication is not currently supported when gRPC mode is enabled",
                cs.getUser().empty());
    }
#endif

    boost::optional<std::string> appname = cs.getAppName();
    std::string errmsg;
    ErrorCodes::Error errcode;
    std::shared_ptr<DBClientBase> conn(
        cs.connect(appname.value_or(MongoURI::kDefaultTestRunnerAppName),
                   errmsg,
                   boost::none,
                   &apiParameters,
                   transientSSLParams,
                   &errcode));
    if (!conn.get()) {
        uasserted(errcode, errmsg);
    }
    // Make the DBClientBase not throw on a StaleConfig error since the shell cannot handle this
    // error and for transactions throwing this error from inside DBClientBase makes the error lose
    // the TransientTransactionError label, which would mislead the external client to not retry the
    // transaction when it should.
    conn->setShouldThrowOnStaleConfigError(false);

    ScriptEngine::runConnectCallback(*conn, host);

    JS::RootedObject thisv(cx);
    scope->getProto<MongoExternalInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    auto encConn = runEncryptedDBClientCallback(conn, args.get(1), thisv, cx);

    auto client = std::make_unique<DBClientWithAutoEncryption>(conn, encConn);
    JS::SetReservedSlot(
        thisv,
        DBClientWithAutoEncryptionSlot,
        JS::PrivateValue(
            scope->trackedNew<std::shared_ptr<DBClientWithAutoEncryption>>(client.release())));

    o.setBoolean(InternedString::slaveOk, false);
    o.setString(InternedString::host, cs.connectionString().toString());
    auto defaultDB = cs.getDatabase() == "" ? "test" : cs.getDatabase();
    o.setString(InternedString::defaultDB, defaultDB);

    // Adds a property to the Mongo connection object.
    boost::optional<bool> retryWrites = cs.getRetryWrites();
    // If retryWrites is not explicitly set in uri, sessions created on this connection default
    // to the global retryWrites value. This is checked in sessions.js by using the injected
    // _shouldRetryWrites() function, which returns true if the --retryWrites flag was passed.
    if (retryWrites) {
        o.setBoolean(InternedString::_retryWrites, retryWrites.value());
    }

    args.rval().setObjectOrNull(thisv);
}

void MongoBase::Functions::getMinWireVersion::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setInt32(conn->getMinWireVersion());
}

void MongoBase::Functions::getMaxWireVersion::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setInt32(conn->getMaxWireVersion());
}

void MongoBase::Functions::isReplicaSetMember::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setBoolean(conn->isReplicaSetMember());
}

void MongoBase::Functions::isMongos::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setBoolean(conn->isMongos());
}

void MongoBase::Functions::isTLS::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setBoolean(conn->isTLS());
}

void MongoBase::Functions::isGRPC::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    args.rval().setBoolean(conn->isGRPC());
}

void MongoBase::Functions::getApiParameters::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    ValueReader(cx, args.rval()).fromBSON(conn->getApiParameters().toBSON(), nullptr, false);
}

void MongoBase::Functions::_startSession::call(JSContext* cx, JS::CallArgs args) {
    auto client = getConnectionRef(args);

    LogicalSessionIdToClient id;
    id.setId(UUID::gen());

    JS::RootedObject obj(cx);
    SessionInfo::make(cx, &obj, client, id.toBSON());

    args.rval().setObjectOrNull(obj.get());
}

void MongoBase::Functions::_setOIDCIdPAuthCallback::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1) {
        uasserted(ErrorCodes::BadValue, "_setOIDCIdPAuthCallBack takes exactly 1 arg");
    }

    if (!args.get(0).isString()) {
        uasserted(ErrorCodes::BadValue,
                  "first argument to _setOIDCIdPAuthCallback must be a stringified function");
    }

    // ValueWriter currently does not have a native way to retrieve a std::function from a JS
    // function. Existing places that require executing JS functions ($where, db.eval) parse the JS
    // function as a raw string and then stash that into a wrapper type such as JsFunction.
    // ScriptingFunction is loaded with the stringified function and then gets invoked by the parent
    // scope. A potential alternative here would be to use JS_ValueToFunction, but that returns a
    // pointer to a JSFunction that is still locally scoped to this function. Hence, we represent
    // the function as a string, stash it into a lambda, and execute it directly when needed.
    std::string stringifiedFn = ValueWriter(cx, args.get(0)).toString();
    SaslOIDCClientConversation::setOIDCIdPAuthCallback(
        [=](StringData userName, StringData idpEndpoint, StringData userCode) {
            std::unique_ptr<Scope> jsScope{getGlobalScriptEngine()->newScope()};
            BSONObj authInfo = BSON("userName" << userName << "userCode" << userCode
                                               << "activationEndpoint" << idpEndpoint);
            ScriptingFunction function = jsScope->createFunction(stringifiedFn.c_str());
            jsScope->invoke(function, nullptr, &authInfo);
        });

    args.rval().setUndefined();
}

void MongoBase::Functions::_refreshAccessToken::call(JSContext* cx, JS::CallArgs args) {
    auto accessToken = uassertStatusOK(SaslOIDCClientConversation::doRefreshFlow());
    ValueReader(cx, args.rval()).fromStringData(accessToken);
}

void MongoExternalInfo::Functions::load::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    for (unsigned i = 0; i < args.length(); ++i) {
        std::string filename = ValueWriter(cx, args.get(i)).toString();

        if (!scope->execFile(filename, false, true)) {
            uasserted(ErrorCodes::BadValue, std::string("error loading js file: ") + filename);
        }
    }

    args.rval().setBoolean(true);
}

void MongoExternalInfo::Functions::quit::call(JSContext* cx, JS::CallArgs args) {
    auto arg = args.get(0);
    quickExit(((arg.isNumber()) && (arg.toNumber() >= 0) && (arg.toNumber() <= 255))
                  ? static_cast<ExitCode>(arg.toNumber())
                  : ExitCode::clean);
}

void MongoExternalInfo::Functions::_forgetReplSet::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_forgetReplSet takes exactly 1 argument, but was given "
                                << args.length());
    }

    std::string rsName = ValueWriter(cx, args.get(0)).toString();
    ReplicaSetMonitorManager::get()->removeMonitor(rsName);

    args.rval().setUndefined();
}

void MongoBase::Functions::getShellPort::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    HostAndPort hostAndPort{conn->getLocalAddress()};
    args.rval().setInt32(hostAndPort.port());
}

}  // namespace mozjs
}  // namespace mongo

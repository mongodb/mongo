
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

#include "mongo/scripting/mozjs/mongo.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/native_sasl_client_session.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/mozjs/cursor.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/session.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec MongoBase::methods[] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(auth, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(close, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(copyDatabaseWithSCRAM, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(cursorFromId, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(cursorHandleFromId, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(find, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getClientRPCProtocols, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getServerRPCProtocols, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(insert, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isReplicaSetConnection, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_markNodeAsFailed, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(logout, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(remove, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(runCommand, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(runCommandWithMetadata, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(setClientRPCProtocols, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(update, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getMinWireVersion, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getMaxWireVersion, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isReplicaSetMember, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isMongos, MongoExternalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(_startSession, MongoExternalInfo),
    JS_FS_END,
};

const char* const MongoBase::className = "Mongo";

const JSFunctionSpec MongoExternalInfo::freeFunctions[4] = {
    MONGO_ATTACH_JS_FUNCTION(_forgetReplSet),
    MONGO_ATTACH_JS_FUNCTION(load),
    MONGO_ATTACH_JS_FUNCTION(quit),
    JS_FS_END,
};

namespace {

const std::shared_ptr<DBClientBase>& getConnectionRef(JS::CallArgs& args) {
    auto ret =
        static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(args.thisv().toObjectOrNull()));
    uassert(
        ErrorCodes::BadValue, "Trying to get connection for closed Mongo object", *ret != nullptr);
    return *ret;
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

void runFireAndForgetCommand(const std::shared_ptr<DBClientBase>& conn,
                             const std::string& database,
                             BSONObj body,
                             const BSONObj& extraFields = {}) {
    auto request = OpMsgRequest::fromDBAndBody(database, body, extraFields);
    conn->runFireAndForgetCommand(request);
}

void setCursor(MozJSImplScope* scope,
               JS::HandleObject target,
               std::unique_ptr<DBClientCursor> cursor,
               JS::CallArgs& args) {
    auto client =
        static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    // Copy the client shared pointer to up the refcount
    JS_SetPrivate(target, scope->trackedNew<CursorInfo::CursorHolder>(std::move(cursor), *client));
}

void setCursorHandle(MozJSImplScope* scope,
                     JS::HandleObject target,
                     NamespaceString ns,
                     long long cursorId,
                     JS::CallArgs& args) {
    auto client =
        static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    // Copy the client shared pointer to up the refcount.
    JS_SetPrivate(
        target,
        scope->trackedNew<CursorHandleInfo::CursorTracker>(std::move(ns), cursorId, *client));
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
                    DBClientBase* origConn,
                    JS::CallArgs& args) {
    ObjectWrapper o(cx, args.rval());
    // If the connection that ran the command is the same as conn, then we set a hidden "_mongo"
    // property on the returned object that is just "this" Mongo object.
    if (resPtr.get() == origConn) {
        setHiddenMongo(cx, args.thisv(), args);
    } else {
        JS::RootedObject newMongo(cx);

        auto scope = getScope(cx);
        scope->getProto<MongoExternalInfo>().newObject(&newMongo);

        auto host = resPtr->getServerAddress();
        JS_SetPrivate(newMongo,
                      scope->trackedNew<std::shared_ptr<DBClientBase>>(std::move(resPtr)));

        ObjectWrapper from(cx, args.thisv());
        ObjectWrapper to(cx, newMongo);
        for (const auto& k : {InternedString::slaveOk, InternedString::defaultDB}) {
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
}  // namespace

void MongoBase::finalize(js::FreeOp* fop, JSObject* obj) {
    auto conn = static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(obj));

    if (conn) {
        getScope(fop)->trackedDelete(conn);
    }
}

void MongoBase::Functions::close::call(JSContext* cx, JS::CallArgs args) {
    getConnection(args);

    auto thisv = args.thisv().toObjectOrNull();
    auto conn = static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(thisv));

    conn->reset();

    args.rval().setUndefined();
}

void MongoBase::Functions::runCommand::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 3)
        uasserted(ErrorCodes::BadValue, "runCommand needs 3 args");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "the database parameter to runCommand must be a string");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue, "the cmdObj parameter to runCommand must be an object");

    if (!args.get(2).isNumber())
        uasserted(ErrorCodes::BadValue, "the options parameter to runCommand must be a number");

    const auto& conn = getConnectionRef(args);

    std::string database = ValueWriter(cx, args.get(0)).toString();

    BSONObj cmdObj = ValueWriter(cx, args.get(1)).toBSON();

    if (isUnacknowledged(cmdObj)) {
        runFireAndForgetCommand(conn, database, cmdObj);
        setHiddenMongo(cx, args);
        returnOk(cx, args);
        return;
    }

    int queryOptions = ValueWriter(cx, args.get(2)).toInt32();
    BSONObj cmdRes;
    auto resTuple = conn->runCommandWithTarget(database, cmdObj, cmdRes, conn, queryOptions);

    // the returned object is not read only as some of our tests depend on modifying it.
    //
    // Also, we make a copy here because we want a copy after we dump cmdRes
    ValueReader(cx, args.rval()).fromBSON(cmdRes.getOwned(), nullptr, false /* read only */);
    setHiddenMongo(cx, std::get<1>(resTuple), conn.get(), args);
}

void MongoBase::Functions::runCommandWithMetadata::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 3)
        uasserted(ErrorCodes::BadValue, "runCommandWithMetadata needs 3 args");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue,
                  "the database parameter to runCommandWithMetadata must be a string");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue,
                  "the metadata argument to runCommandWithMetadata must be an object");

    if (!args.get(2).isObject())
        uasserted(ErrorCodes::BadValue,
                  "the commandArgs argument to runCommandWithMetadata must be an object");

    std::string database = ValueWriter(cx, args.get(0)).toString();
    BSONObj metadata = ValueWriter(cx, args.get(1)).toBSON();
    BSONObj commandArgs = ValueWriter(cx, args.get(2)).toBSON();

    const auto& conn = getConnectionRef(args);

    if (isUnacknowledged(commandArgs)) {
        runFireAndForgetCommand(conn, database, commandArgs, metadata);
        setHiddenMongo(cx, args);
        returnOk(cx, args);
        return;
    }

    auto resTuple = conn->runCommandWithTarget(
        OpMsgRequest::fromDBAndBody(database, commandArgs, metadata), conn);
    auto res = std::move(std::get<0>(resTuple));

    BSONObjBuilder mergedResultBob;
    mergedResultBob.append("commandReply", res->getCommandReply());

    ValueReader(cx, args.rval()).fromBSON(mergedResultBob.obj(), nullptr, false);
    setHiddenMongo(cx, std::get<1>(resTuple), conn.get(), args);
}

void MongoBase::Functions::find::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 7)
        uasserted(ErrorCodes::BadValue, "find needs 7 args");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue, "needs to be an object");

    auto conn = getConnection(args);

    std::string ns = ValueWriter(cx, args.get(0)).toString();

    BSONObj fields;
    BSONObj q = ValueWriter(cx, args.get(1)).toBSON();

    bool haveFields = false;

    if (args.get(2).isObject()) {
        JS::RootedObject obj(cx, args.get(2).toObjectOrNull());

        ObjectWrapper(cx, obj).enumerate([&](jsid) {
            haveFields = true;
            return false;
        });
    }

    if (haveFields)
        fields = ValueWriter(cx, args.get(2)).toBSON();

    int nToReturn = ValueWriter(cx, args.get(3)).toInt32();
    int nToSkip = ValueWriter(cx, args.get(4)).toInt32();
    int batchSize = ValueWriter(cx, args.get(5)).toInt32();
    int options = ValueWriter(cx, args.get(6)).toInt32();

    // The shell only calls this method when it wants to test OP_QUERY.
    options |= DBClientCursor::QueryOptionLocal_forceOpQuery;

    std::unique_ptr<DBClientCursor> cursor(conn->query(NamespaceString(ns),
                                                       q,
                                                       nToReturn,
                                                       nToSkip,
                                                       haveFields ? &fields : NULL,
                                                       options,
                                                       batchSize));
    if (!cursor.get()) {
        uasserted(ErrorCodes::InternalError, "error doing query: failed");
    }

    JS::RootedObject c(cx);
    scope->getProto<CursorInfo>().newObject(&c);

    setCursor(scope, c, std::move(cursor), args);

    args.rval().setObjectOrNull(c);
}

void MongoBase::Functions::insert::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 3)
        uasserted(ErrorCodes::BadValue, "insert needs 3 args");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue, "attempted to insert a non-object");

    ObjectWrapper o(cx, args.thisv());

    if (o.hasField(InternedString::readOnly) && o.getBoolean(InternedString::readOnly))
        uasserted(ErrorCodes::BadValue, "js db in read only mode");

    auto conn = getConnection(args);

    std::string ns = ValueWriter(cx, args.get(0)).toString();

    int flags = ValueWriter(cx, args.get(2)).toInt32();

    auto addId = [cx, scope](JS::HandleValue value) {
        if (!value.isObject())
            uasserted(ErrorCodes::BadValue, "attempted to insert a non-object type");

        JS::RootedObject elementObj(cx, value.toObjectOrNull());

        ObjectWrapper ele(cx, elementObj);

        if (!ele.hasField(InternedString::_id)) {
            JS::RootedValue value(cx);
            scope->getProto<OIDInfo>().newInstance(&value);
            ele.setValue(InternedString::_id, value);
        }

        return ValueWriter(cx, value).toBSON();
    };

    Message toSend;
    if (args.get(1).isObject()) {
        bool isArray;

        if (!JS_IsArrayObject(cx, args.get(1), &isArray)) {
            uasserted(ErrorCodes::BadValue, "Failure to check is object an array");
        }

        if (isArray) {
            JS::RootedObject obj(cx, args.get(1).toObjectOrNull());
            ObjectWrapper array(cx, obj);

            std::vector<BSONObj> bos;

            bool foundElement = false;

            array.enumerate([&](JS::HandleId id) {
                foundElement = true;

                JS::RootedValue value(cx);
                array.getValue(id, &value);

                bos.push_back(addId(value));

                return true;
            });

            if (!foundElement)
                uasserted(ErrorCodes::BadValue, "attempted to insert an empty array");

            toSend = makeInsertMessage(ns, bos.data(), bos.size(), flags);
        } else {
            toSend = makeInsertMessage(ns, addId(args.get(1)));
        }
    } else {
        toSend = makeInsertMessage(ns, addId(args.get(1)));
    }

    invariant(!toSend.empty());
    conn->say(toSend);

    args.rval().setUndefined();
}

void MongoBase::Functions::remove::call(JSContext* cx, JS::CallArgs args) {
    if (!(args.length() == 2 || args.length() == 3))
        uasserted(ErrorCodes::BadValue, "remove needs 2 or 3 args");

    if (!(args.get(1).isObject()))
        uasserted(ErrorCodes::BadValue, "attempted to remove a non-object");

    ObjectWrapper o(cx, args.thisv());

    if (o.hasOwnField(InternedString::readOnly) && o.getBoolean(InternedString::readOnly))
        uasserted(ErrorCodes::BadValue, "js db in read only mode");

    auto conn = getConnection(args);
    std::string ns = ValueWriter(cx, args.get(0)).toString();

    BSONObj bson = ValueWriter(cx, args.get(1)).toBSON();

    bool justOne = false;
    if (args.length() > 2) {
        justOne = args.get(2).toBoolean();
    }

    auto toSend = makeRemoveMessage(ns, bson, justOne ? RemoveOption_JustOne : 0);
    conn->say(toSend);
    args.rval().setUndefined();
}

void MongoBase::Functions::update::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() < 3)
        uasserted(ErrorCodes::BadValue, "update needs at least 3 args");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue, "1st param to update has to be an object");

    if (!args.get(2).isObject())
        uasserted(ErrorCodes::BadValue, "2nd param to update has to be an object");

    ObjectWrapper o(cx, args.thisv());

    if (o.hasOwnField(InternedString::readOnly) && o.getBoolean(InternedString::readOnly))
        uasserted(ErrorCodes::BadValue, "js db in read only mode");

    auto conn = getConnection(args);
    std::string ns = ValueWriter(cx, args.get(0)).toString();

    BSONObj q1 = ValueWriter(cx, args.get(1)).toBSON();
    BSONObj o1 = ValueWriter(cx, args.get(2)).toBSON();

    bool upsert = args.length() > 3 && args.get(3).isBoolean() && args.get(3).toBoolean();
    bool multi = args.length() > 4 && args.get(4).isBoolean() && args.get(4).toBoolean();

    auto toSend = makeUpdateMessage(
        ns, q1, o1, (upsert ? UpdateOption_Upsert : 0) | (multi ? UpdateOption_Multi : 0));
    conn->say(toSend);
    args.rval().setUndefined();
}

void MongoBase::Functions::auth::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);
    uassert(ErrorCodes::BadValue, "no connection", conn);
    uassert(ErrorCodes::BadValue, "mongoAuth takes exactly 1 object argument", args.length() == 1);

    conn->auth(ValueWriter(cx, args.get(0)).toBSON());
    args.rval().setBoolean(true);
}

void MongoBase::Functions::logout::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "logout needs 1 arg");

    BSONObj ret;

    std::string db = ValueWriter(cx, args.get(0)).toString();

    auto conn = getConnection(args);
    if (conn) {
        conn->logout(db, ret);
    }

    // Make a copy because I want to insulate us from whether conn->logout
    // writes an owned bson or not
    ValueReader(cx, args.rval()).fromBSON(ret.getOwned(), nullptr, false);
}

void MongoBase::Functions::cursorFromId::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (!(args.length() == 2 || args.length() == 3))
        uasserted(ErrorCodes::BadValue, "cursorFromId needs 2 or 3 args");

    if (!scope->getProto<NumberLongInfo>().instanceOf(args.get(1)))
        uasserted(ErrorCodes::BadValue, "2nd arg must be a NumberLong");

    if (!(args.get(2).isNumber() || args.get(2).isUndefined()))
        uasserted(ErrorCodes::BadValue, "3rd arg must be a js Number");

    auto conn = getConnection(args);

    std::string ns = ValueWriter(cx, args.get(0)).toString();

    long long cursorId = NumberLongInfo::ToNumberLong(cx, args.get(1));

    // The shell only calls this method when it wants to test OP_GETMORE.
    auto cursor = stdx::make_unique<DBClientCursor>(
        conn, NamespaceString(ns), cursorId, 0, DBClientCursor::QueryOptionLocal_forceOpQuery);

    if (args.get(2).isNumber())
        cursor->setBatchSize(ValueWriter(cx, args.get(2)).toInt32());

    JS::RootedObject c(cx);
    scope->getProto<CursorInfo>().newObject(&c);

    setCursor(scope, c, std::move(cursor), args);

    args.rval().setObjectOrNull(c);
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

    setCursorHandle(scope, c, NamespaceString(ns), cursorId, args);

    args.rval().setObjectOrNull(c);
}

void MongoBase::Functions::copyDatabaseWithSCRAM::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (!conn)
        uasserted(ErrorCodes::BadValue, "no connection");

    if (args.length() != 6)
        uasserted(ErrorCodes::BadValue, "copyDatabase needs 6 arg");

    // copyDatabase(fromdb, todb, fromhost, username, password);
    std::string fromDb = ValueWriter(cx, args.get(0)).toString();
    std::string toDb = ValueWriter(cx, args.get(1)).toString();
    std::string fromHost = ValueWriter(cx, args.get(2)).toString();
    std::string user = ValueWriter(cx, args.get(3)).toString();
    std::string password = ValueWriter(cx, args.get(4)).toString();
    bool slaveOk = ValueWriter(cx, args.get(5)).toBoolean();

    std::string hashedPwd = DBClientBase::createPasswordDigest(user, password);

    std::unique_ptr<SaslClientSession> session(new NativeSaslClientSession());

    session->setParameter(SaslClientSession::parameterMechanism, "SCRAM-SHA-1");
    session->setParameter(SaslClientSession::parameterUser, user);
    session->setParameter(SaslClientSession::parameterPassword, hashedPwd);
    session->initialize().transitional_ignore();

    BSONObj saslFirstCommandPrefix =
        BSON("copydbsaslstart" << 1 << "fromhost" << fromHost << "fromdb" << fromDb
                               << saslCommandMechanismFieldName
                               << "SCRAM-SHA-1");

    BSONObj saslFollowupCommandPrefix = BSON(
        "copydb" << 1 << "fromhost" << fromHost << "fromdb" << fromDb << "todb" << toDb << "slaveOk"
                 << slaveOk);

    BSONObj saslCommandPrefix = saslFirstCommandPrefix;
    BSONObj inputObj = BSON(saslCommandPayloadFieldName << "");
    bool isServerDone = false;

    while (!session->isSuccess()) {
        std::string payload;
        BSONType type;

        Status status = saslExtractPayload(inputObj, &payload, &type);
        uassertStatusOK(status);

        std::string responsePayload;
        status = session->step(payload, &responsePayload);
        uassertStatusOK(status);

        BSONObjBuilder commandBuilder(std::move(saslCommandPrefix));
        commandBuilder.appendBinData(saslCommandPayloadFieldName,
                                     static_cast<int>(responsePayload.size()),
                                     BinDataGeneral,
                                     responsePayload.c_str());
        BSONElement conversationId = inputObj[saslCommandConversationIdFieldName];
        if (!conversationId.eoo())
            commandBuilder.append(conversationId);

        BSONObj command = commandBuilder.obj();

        bool ok = conn->runCommand("admin", command, inputObj);
        if (!ok) {
            ValueReader(cx, args.rval()).fromBSON(inputObj, nullptr, true);
            return;
        }

        isServerDone = inputObj[saslCommandDoneFieldName].trueValue();
        saslCommandPrefix = saslFollowupCommandPrefix;
    }

    if (!isServerDone) {
        uasserted(ErrorCodes::InternalError, "copydb client finished before server.");
    }

    ValueReader(cx, args.rval()).fromBSON(inputObj, nullptr, true);
}

void MongoBase::Functions::getClientRPCProtocols::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (args.length() != 0)
        uasserted(ErrorCodes::BadValue, "getClientRPCProtocols takes no args");

    auto clientRPCProtocols = rpc::toString(conn->getClientRPCProtocols());
    uassertStatusOK(clientRPCProtocols);

    auto protoStr = clientRPCProtocols.getValue().toString();

    ValueReader(cx, args.rval()).fromStringData(protoStr);
}

void MongoBase::Functions::setClientRPCProtocols::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "setClientRPCProtocols needs 1 arg");
    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "first argument to setClientRPCProtocols must be a string");

    std::string rpcProtosStr = ValueWriter(cx, args.get(0)).toString();

    auto clientRPCProtocols = rpc::parseProtocolSet(rpcProtosStr);
    uassertStatusOK(clientRPCProtocols);

    conn->setClientRPCProtocols(clientRPCProtocols.getValue());

    args.rval().setUndefined();
}

void MongoBase::Functions::getServerRPCProtocols::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (args.length() != 0)
        uasserted(ErrorCodes::BadValue, "getServerRPCProtocols takes no args");

    auto serverRPCProtocols = rpc::toString(conn->getServerRPCProtocols());
    uassertStatusOK(serverRPCProtocols);

    auto protoStr = serverRPCProtocols.getValue().toString();

    ValueReader(cx, args.rval()).fromStringData(protoStr);
}

void MongoBase::Functions::isReplicaSetConnection::call(JSContext* cx, JS::CallArgs args) {
    auto conn = getConnection(args);

    if (args.length() != 0) {
        uasserted(ErrorCodes::BadValue, "isReplicaSetConnection takes no args");
    }

    args.rval().setBoolean(conn->type() == ConnectionString::ConnectionType::SET);
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

    auto* rsConn = dynamic_cast<DBClientReplicaSet*>(getConnection(args));
    if (!rsConn) {
        uasserted(ErrorCodes::BadValue, "connection object isn't a replica set connection");
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

void MongoExternalInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    std::string host("127.0.0.1");

    if (args.length() > 0 && args.get(0).isString()) {
        host = ValueWriter(cx, args.get(0)).toString();
    }

    auto cs = uassertStatusOK(MongoURI::parse(host));

    boost::optional<std::string> appname = cs.getAppName();
    std::string errmsg;
    std::unique_ptr<DBClientBase> conn(cs.connect(appname.value_or("MongoDB Shell"), errmsg));

    if (!conn.get()) {
        uasserted(ErrorCodes::InternalError, errmsg);
    }

    ScriptEngine::runConnectCallback(*conn);

    JS::RootedObject thisv(cx);
    scope->getProto<MongoExternalInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    JS_SetPrivate(thisv, scope->trackedNew<std::shared_ptr<DBClientBase>>(conn.release()));

    o.setBoolean(InternedString::slaveOk, false);
    o.setString(InternedString::host, cs.connectionString().toString());
    auto defaultDB = cs.getDatabase() == "" ? "test" : cs.getDatabase();
    o.setString(InternedString::defaultDB, defaultDB);

    // Adds a property to the Mongo connection object.
    boost::optional<bool> retryWrites = cs.getRetryWrites();
    // If retryWrites is not explicitly set in uri, sessions created on this connection default to
    // the global retryWrites value. This is checked in sessions.js by using the injected
    // _shouldRetryWrites() function, which returns true if the --retryWrites flag was passed.
    if (retryWrites) {
        o.setBoolean(InternedString::_retryWrites, retryWrites.get());
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

void MongoBase::Functions::_startSession::call(JSContext* cx, JS::CallArgs args) {
    auto client =
        static_cast<std::shared_ptr<DBClientBase>*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    LogicalSessionIdToClient id;
    id.setId(UUID::gen());

    JS::RootedObject obj(cx);
    SessionInfo::make(cx, &obj, *client, id.toBSON());

    args.rval().setObjectOrNull(obj.get());
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
    quickExit(args.get(0).isNumber() ? args.get(0).toNumber() : 0);
}

void MongoExternalInfo::Functions::_forgetReplSet::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_forgetReplSet takes exactly 1 argument, but was given "
                                << args.length());
    }

    std::string rsName = ValueWriter(cx, args.get(0)).toString();
    globalRSMonitorManager.removeMonitor(rsName);

    args.rval().setUndefined();
}

}  // namespace mozjs
}  // namespace mongo

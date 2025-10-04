/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/shell/encrypted_dbclient_base.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/read_preference.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/scripting/mozjs/bindata.h"
#include "mongo/scripting/mozjs/code.h"
#include "mongo/scripting/mozjs/db.h"
#include "mongo/scripting/mozjs/dbcollection.h"
#include "mongo/scripting/mozjs/dbref.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/maxkey.h"
#include "mongo/scripting/mozjs/minkey.h"
#include "mongo/scripting/mozjs/mongo.h"
#include "mongo/scripting/mozjs/numberdecimal.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wraptype.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <new>
#include <stack>

#include <jsapi.h>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/Object.h>
#include <js/RootingAPI.h>
#include <js/TracingAPI.h>
#include <js/TypeDecls.h>
#include <js/Value.h>
#include <js/ValueArray.h>

namespace mongo {

namespace {
constexpr Duration kCacheInvalidationTime = Minutes(1);
constexpr StringData compactCmdName = "compact"_sd;
constexpr StringData cleanupCmdName = "cleanup"_sd;

ImplicitEncryptedDBClientCallback* implicitEncryptedDBClientCallback{nullptr};


}  // namespace

void setImplicitEncryptedDBClientCallback(ImplicitEncryptedDBClientCallback* callback) {
    implicitEncryptedDBClientCallback = callback;
}

static void validateCollection(JSContext* cx, JS::HandleValue value) {
    uassert(ErrorCodes::BadValue,
            "Collection object must be provided to ClientSideFLEOptions",
            !(value.isNull() || value.isUndefined()));

    JS::RootedValue coll(cx, value);

    uassert(31043,
            "The collection object in ClientSideFLEOptions is invalid",
            mozjs::getScope(cx)->getProto<mozjs::DBCollectionInfo>().instanceOf(coll));
}

EncryptedDBClientBase::EncryptedDBClientBase(std::shared_ptr<DBClientBase> conn,
                                             ClientSideFLEOptions encryptionOptions,
                                             JS::HandleValue collection,
                                             JSContext* cx)
    : _conn(std::move(conn)), _encryptionOptions(std::move(encryptionOptions)), _cx(cx) {
    validateCollection(cx, collection);
    _collection = JS::Heap<JS::Value>(collection);
};

std::string EncryptedDBClientBase::getServerAddress() const {
    return _conn->getServerAddress();
}

std::string EncryptedDBClientBase::getLocalAddress() const {
    return _conn->getLocalAddress();
}

Message EncryptedDBClientBase::_call(Message& toSend, std::string* actualServer) {
    return _conn->call(toSend, actualServer);
}

void EncryptedDBClientBase::say(Message& toSend, bool isRetry, std::string* actualServer) {
    return _conn->say(toSend, isRetry, actualServer);
}

BSONObj EncryptedDBClientBase::encryptDecryptCommand(const BSONObj& object,
                                                     bool encrypt,
                                                     const DatabaseName& dbName) {
    std::stack<std::pair<BSONObjIterator, BSONObjBuilder>> frameStack;

    // The encryptDecryptCommand frameStack requires a guard because  if encryptMarking or
    // decrypt payload throw an exception, the stack's destructor will fire. Because a stack's
    // variables are not guaranteed to be destroyed in any order, we need to add a guard
    // to ensure the stack is destroyed in order.
    const ScopeGuard frameStackGuard([&] {
        while (!frameStack.empty()) {
            frameStack.pop();
        }
    });

    frameStack.emplace(BSONObjIterator(object), BSONObjBuilder());

    while (frameStack.size() > 1 || frameStack.top().first.more()) {
        uassert(31096,
                "Object too deep to be encrypted. Exceeded stack depth.",
                frameStack.size() < BSONDepth::kDefaultMaxAllowableDepth);
        auto& [iterator, builder] = frameStack.top();
        if (iterator.more()) {
            BSONElement elem = iterator.next();
            if (elem.type() == BSONType::object) {
                frameStack.emplace(BSONObjIterator(elem.Obj()),
                                   BSONObjBuilder(builder.subobjStart(elem.fieldNameStringData())));
            } else if (elem.type() == BSONType::array) {
                frameStack.emplace(
                    BSONObjIterator(elem.Obj()),
                    BSONObjBuilder(builder.subarrayStart(elem.fieldNameStringData())));
            } else if (elem.isBinData(BinDataType::Encrypt)) {
                int len;
                const char* data(elem.binData(len));
                uassert(31178, "Invalid intentToEncrypt object from Query Analyzer", len >= 1);
                if ((*data == kRandomEncryptionBit || *data == kDeterministicEncryptionBit) &&
                    !encrypt) {
                    ConstDataRange dataCursor(data, len);
                    decryptPayload(dataCursor, &builder, elem.fieldNameStringData());
                } else if (*data == kIntentToEncryptBit && encrypt) {
                    BSONObj obj = BSONObj(data + 1);
                    encryptMarking(obj, &builder, elem.fieldNameStringData());
                } else {
                    builder.append(elem);
                }
            } else {
                builder.append(elem);
            }
        } else {
            frameStack.pop();
        }
    }
    invariant(frameStack.size() == 1);
    // Append '$db' which shouldn't contain tenantid.
    frameStack.top().second.append("$db", dbName.toString_forTest());
    return frameStack.top().second.obj();
}

void EncryptedDBClientBase::encryptMarking(const BSONObj& elem,
                                           BSONObjBuilder* builder,
                                           StringData elemName) {
    MONGO_UNREACHABLE;
}

void EncryptedDBClientBase::decryptPayload(ConstDataRange data,
                                           BSONObjBuilder* builder,
                                           StringData elemName) {
    invariant(builder);
    uassert(ErrorCodes::BadValue, "Invalid decryption blob", data.length() > kAssociatedDataLength);

    FLEDecryptionFrame dataFrame = createDecryptionFrame(data);
    auto plaintext = dataFrame.getPlaintext();

    // extract type byte
    const uint8_t bsonType = dataFrame.getBSONType();
    BSONObj decryptedObj = validateBSONElement(plaintext, bsonType);
    if (bsonType == stdx::to_underlying(BSONType::object)) {
        builder->append(elemName, decryptedObj);
    } else {
        builder->appendAs(decryptedObj.firstElement(), elemName);
    }
}

EncryptedDBClientBase::RunCommandReturn EncryptedDBClientBase::processResponseFLE1(
    EncryptedDBClientBase::RunCommandReturn result, const DatabaseName& dbName) {
    auto rawReply = result.returnReply->getCommandReply();
    return prepareReply(std::move(result), encryptDecryptCommand(rawReply, false, dbName));
}

EncryptedDBClientBase::RunCommandReturn EncryptedDBClientBase::processResponseFLE2(
    EncryptedDBClientBase::RunCommandReturn result) {
    auto rawReply = result.returnReply->getCommandReply();
    return prepareReply(std::move(result), FLEClientCrypto::decryptDocument(rawReply, this));
}

EncryptedDBClientBase::RunCommandReturn EncryptedDBClientBase::prepareReply(
    EncryptedDBClientBase::RunCommandReturn result, BSONObj decryptedDoc) {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(StatusWith<BSONObj>(decryptedDoc));
    auto msg = replyBuilder.done();

    auto host = _conn->getServerAddress();
    auto reply = _conn->parseCommandReplyMessage(host, msg);

    return EncryptedDBClientBase::RunCommandReturn({std::move(reply), result});
}

EncryptedDBClientBase::RunCommandReturn EncryptedDBClientBase::doRunCommand(
    EncryptedDBClientBase::RunCommandParams params) {
    if (params.type == EncryptedDBClientBase::RunCommandConnectionType::rawPtr) {
        return EncryptedDBClientBase::RunCommandReturn(
            _conn->runCommandWithTarget(std::move(params.request)));
    }
    invariant(params.conn);
    return EncryptedDBClientBase::RunCommandReturn(
        _conn->runCommandWithTarget(std::move(params.request), params.conn));
}

EncryptedDBClientBase::RunCommandReturn EncryptedDBClientBase::handleEncryptionRequest(
    EncryptedDBClientBase::RunCommandParams params) {
    auto& request = params.request;
    auto commandName = std::string{request.getCommandName()};
    const DatabaseName dbName = request.parseDbName();

    if (std::find(kEncryptedCommands.begin(), kEncryptedCommands.end(), StringData(commandName)) ==
        std::end(kEncryptedCommands)) {
        return doRunCommand(std::move(params));
    }

    EncryptedDBClientBase::RunCommandReturn result(doRunCommand(std::move(params)));
    return processResponseFLE1(processResponseFLE2(std::move(result)), dbName);
}

std::pair<rpc::UniqueReply, DBClientBase*> EncryptedDBClientBase::runCommandWithTarget(
    OpMsgRequest request) {
    EncryptedDBClientBase::RunCommandParams params(request);
    auto result = handleEncryptionRequest(std::move(params));
    auto returnConn = get<DBClientBase*>(result.returnConn);
    return {std::move(result.returnReply), returnConn};
}

std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>>
EncryptedDBClientBase::runCommandWithTarget(OpMsgRequest request,
                                            std::shared_ptr<DBClientBase> conn) {
    EncryptedDBClientBase::RunCommandParams params(request, conn);
    auto result = handleEncryptionRequest(std::move(params));
    auto returnConn = get<std::shared_ptr<DBClientBase>>(result.returnConn);
    return {std::move(result.returnReply), returnConn};
}

/**
 *
 * This function reads the data from the CDR and returns a copy
 * constructed and owned BSONObject.
 *
 */
BSONObj EncryptedDBClientBase::validateBSONElement(ConstDataRange out, uint8_t bsonType) {
    if (bsonType == stdx::to_underlying(BSONType::object)) {
        ConstDataRangeCursor cdc = ConstDataRangeCursor(out);
        BSONObj valueObj;

        valueObj = cdc.readAndAdvance<Validated<BSONObj>>();
        return valueObj.getOwned();
    } else {
        auto valueString = "value"_sd;

        // The size here is to construct a new BSON document and validate the
        // total size of the object. The first four bytes is for the size of an
        // int32_t, then a space for the type of the first element, then the space
        // for the value string and the the 0x00 terminated field name, then the
        // size of the actual data, then the last byte for the end document character,
        // also 0x00.
        size_t docLength = sizeof(int32_t) + 1 + valueString.size() + 1 + out.length() + 1;
        BufBuilder builder;
        builder.reserveBytes(docLength);

        uassert(ErrorCodes::BadValue,
                "invalid decryption value",
                docLength < std::numeric_limits<int32_t>::max());

        builder.appendNum(static_cast<uint32_t>(docLength));
        builder.appendChar(static_cast<uint8_t>(bsonType));
        builder.appendCStr(valueString);
        builder.appendBuf(out.data(), out.length());
        builder.appendChar('\0');

        ConstDataRangeCursor cdc =
            ConstDataRangeCursor(ConstDataRange(builder.buf(), builder.len()));
        BSONObj elemWrapped = cdc.readAndAdvance<Validated<BSONObj>>();
        return elemWrapped.getOwned();
    }
}

std::string EncryptedDBClientBase::toString() const {
    return _conn->toString();
}

int EncryptedDBClientBase::getMinWireVersion() {
    return _conn->getMinWireVersion();
}

int EncryptedDBClientBase::getMaxWireVersion() {
    return _conn->getMaxWireVersion();
}

void EncryptedDBClientBase::generateDataKey(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 2) {
        uasserted(ErrorCodes::BadValue, "generateDataKey requires 2 arg");
    }

    if (!args.get(0).isString()) {
        uasserted(ErrorCodes::BadValue, "1st param to generateDataKey has to be a string");
    }

    if (!args.get(1).isString() && !args.get(1).isObject()) {
        uasserted(ErrorCodes::BadValue,
                  "2nd param to generateDataKey has to be a string or object");
    }

    std::string kmsProvider = mozjs::ValueWriter(cx, args.get(0)).toString();

    std::unique_ptr<KMSService> kmsService = KMSServiceController::createFromClient(
        kmsProvider, _encryptionOptions.getKmsProviders().toBSON());

    SecureVector<uint8_t> dataKey(crypto::kFieldLevelEncryptionKeySize);
    auto res = crypto::engineRandBytes({dataKey->data(), dataKey->size()});
    uassert(31042, "Error generating data key: " + res.codeString(), res.isOK());


    if (args.get(1).isString()) {
        std::string clientMasterKey = mozjs::ValueWriter(cx, args.get(1)).toString();

        BSONObj obj = kmsService->encryptDataKeyByString(
            ConstDataRange(dataKey->data(), dataKey->size()), clientMasterKey);

        mozjs::ValueReader(cx, args.rval()).fromBSON(obj, nullptr, false);
    } else {
        BSONObj clientMasterKey = mozjs::ValueWriter(cx, args.get(1)).toBSON();

        BSONObj obj = kmsService->encryptDataKeyByBSONObj(
            ConstDataRange(dataKey->data(), dataKey->size()), clientMasterKey);

        mozjs::ValueReader(cx, args.rval()).fromBSON(obj, nullptr, false);
    }
}

void EncryptedDBClientBase::getDataKeyCollection(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 0) {
        uasserted(ErrorCodes::BadValue, "getDataKeyCollection does not take any params");
    }
    args.rval().set(_collection.get());
}

void EncryptedDBClientBase::encrypt(mozjs::MozJSImplScope* scope,
                                    JSContext* cx,
                                    JS::CallArgs args) {
    // Input Validation
    uassert(ErrorCodes::BadValue, "encrypt requires 3 args", args.length() == 3);

    if (!(args.get(1).isObject() || args.get(1).isString() || args.get(1).isNumber() ||
          args.get(1).isBoolean())) {
        uasserted(ErrorCodes::BadValue,
                  "Second parameter must be an object, string, number, or bool");
    }

    uassert(ErrorCodes::BadValue, "Third parameter must be a string", args.get(2).isString());
    auto algorithmStr = mozjs::ValueWriter(cx, args.get(2)).toString();
    FleAlgorithmInt algorithm;

    if (StringData(algorithmStr) == FleAlgorithm_serializer(FleAlgorithmEnum::kRandom)) {
        algorithm = FleAlgorithmInt::kRandom;
    } else if (StringData(algorithmStr) ==
               FleAlgorithm_serializer(FleAlgorithmEnum::kDeterministic)) {
        algorithm = FleAlgorithmInt::kDeterministic;
    } else {
        uasserted(ErrorCodes::BadValue, "Third parameter must be the FLE Algorithm type");
    }

    // Extract the UUID from the callArgs
    auto binData = getBinDataArg(scope, cx, args, 0, BinDataType::newUUID);
    UUID uuid = UUID::fromCDR(ConstDataRange(binData.data(), binData.size()));
    BSONType bsonType = BSONType::eoo;

    BufBuilder plaintextBuilder;
    if (args.get(1).isObject()) {
        JS::RootedObject rootedObj(cx, &args.get(1).toObject());
        auto jsclass = JS::GetClass(rootedObj);

        if (strcmp(jsclass->name, "Object") == 0 || strcmp(jsclass->name, "Array") == 0) {
            uassert(ErrorCodes::BadValue,
                    "Cannot deterministically encrypt object or array types.",
                    algorithm != FleAlgorithmInt::kDeterministic);

            // If it is a JS Object, then we can extract all the information by simply calling
            // ValueWriter.toBSON and setting the type bit, which is what is happening below.
            BSONObj valueObj = mozjs::ValueWriter(cx, args.get(1)).toBSON();
            plaintextBuilder.appendBuf(valueObj.objdata(), valueObj.objsize());
            if (strcmp(jsclass->name, "Array") == 0) {
                bsonType = BSONType::array;
            } else {
                bsonType = BSONType::object;
            }

        } else if (scope->getProto<mozjs::MinKeyInfo>().getJSClass() == jsclass ||
                   scope->getProto<mozjs::MaxKeyInfo>().getJSClass() == jsclass ||
                   scope->getProto<mozjs::DBRefInfo>().getJSClass() == jsclass) {
            uasserted(ErrorCodes::BadValue, "Second parameter cannot be MinKey, MaxKey, or DBRef");
        } else {
            if (scope->getProto<mozjs::BinDataInfo>().getJSClass() == jsclass) {
                mozjs::ObjectWrapper o(cx, args.get(1));
                auto binType = BinDataType(o.getNumberInt(mozjs::InternedString::type));
                uassert(ErrorCodes::BadValue,
                        "Cannot encrypt BinData subtype 2.",
                        binType != BinDataType::ByteArrayDeprecated);
            }
            if (scope->getProto<mozjs::NumberDecimalInfo>().getJSClass() == jsclass) {
                uassert(ErrorCodes::BadValue,
                        "Cannot deterministically encrypt NumberDecimal type objects.",
                        algorithm != FleAlgorithmInt::kDeterministic);
            }

            if (scope->getProto<mozjs::CodeInfo>().getJSClass() == jsclass) {
                uassert(ErrorCodes::BadValue,
                        "Cannot deterministically encrypt Code type objects.",
                        algorithm != FleAlgorithmInt::kDeterministic);
            }

            // If it is one of our Mongo defined types, then we have to use the ValueWriter
            // writeThis function, which takes in a set of WriteFieldRecursionFrames (setting
            // a limit on how many times we can recursively dig into an object's nested
            // structure) and writes the value out to a BSONObjBuilder. We can then extract
            // that information from the object by building it and pulling out the first
            // element, which is the object we are trying to get.
            mozjs::ObjectWrapper::WriteFieldRecursionFrames frames;
            frames.emplace(cx, rootedObj.get(), nullptr, StringData{});
            BSONObjBuilder builder;
            mozjs::ValueWriter(cx, args.get(1)).writeThis(&builder, "value"_sd, &frames);

            BSONObj object = builder.obj();
            auto elem = object.getField("value"_sd);

            plaintextBuilder.appendBuf(elem.value(), elem.valuesize());
            bsonType = elem.type();
        }

    } else if (args.get(1).isString()) {
        std::string valueStr = mozjs::ValueWriter(cx, args.get(1)).toString();
        if (valueStr.size() + 1 > std::numeric_limits<uint32_t>::max()) {
            uasserted(ErrorCodes::BadValue, "Plaintext string to encrypt too long.");
        }

        plaintextBuilder.appendNum(static_cast<uint32_t>(valueStr.size() + 1));
        plaintextBuilder.appendStrBytesAndNul(valueStr);
        bsonType = BSONType::string;

    } else if (args.get(1).isNumber()) {
        uassert(ErrorCodes::BadValue,
                "Cannot deterministically encrypt Floating Point numbers.",
                algorithm != FleAlgorithmInt::kDeterministic);

        double valueNum = mozjs::ValueWriter(cx, args.get(1)).toNumber();
        plaintextBuilder.appendNum(valueNum);
        bsonType = BSONType::numberDouble;
    } else if (args.get(1).isBoolean()) {
        uassert(ErrorCodes::BadValue,
                "Cannot deterministically encrypt booleans.",
                algorithm != FleAlgorithmInt::kDeterministic);

        bool boolean = mozjs::ValueWriter(cx, args.get(1)).toBoolean();
        if (boolean) {
            plaintextBuilder.appendChar(0x01);
        } else {
            plaintextBuilder.appendChar(0x00);
        }
        bsonType = BSONType::boolean;
    } else {
        uasserted(ErrorCodes::BadValue, "Cannot encrypt valuetype provided.");
    }

    ConstDataRange plaintext(plaintextBuilder.buf(), plaintextBuilder.len());

    FLEEncryptionFrame encryptionFrame =
        createEncryptionFrame(getDataKey(uuid), algorithm, uuid, bsonType, plaintext);

    // Prepare the return value
    ConstDataRange ciphertextBlob(encryptionFrame.get());
    std::string blobStr =
        base64::encode(StringData(ciphertextBlob.data(), ciphertextBlob.length()));
    JS::RootedValueArray<2> arr(cx);

    arr[0].setInt32(BinDataType::Encrypt);
    mozjs::ValueReader(cx, arr[1]).fromStringData(blobStr);
    scope->getProto<mozjs::BinDataInfo>().newInstance(arr, args.rval());
}

void EncryptedDBClientBase::decrypt(mozjs::MozJSImplScope* scope,
                                    JSContext* cx,
                                    JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "decrypt requires one argument", args.length() == 1);
    uassert(ErrorCodes::BadValue,
            "decrypt argument must be a BinData subtype Encrypt object",
            args.get(0).isObject());

    uassert(ErrorCodes::BadValue,
            "decrypt argument must be a BinData subtype Encrypt object",
            scope->getProto<mozjs::BinDataInfo>().instanceOf(args.get(0)));

    JS::RootedObject obj(cx, &args.get(0).get().toObject());
    std::vector<uint8_t> data = getBinDataArg(scope, cx, args, 0, BinDataType::Encrypt);

    ConstDataRange ciphertextBlob(data);

    FLEDecryptionFrame dataFrame = createDecryptionFrame(ciphertextBlob);

    const uint8_t bsonType = dataFrame.getBSONType();
    BSONObj parent;
    BSONObj decryptedObj = validateBSONElement(dataFrame.getPlaintext(), bsonType);
    if (bsonType == stdx::to_underlying(BSONType::object)) {
        mozjs::ValueReader(cx, args.rval()).fromBSON(decryptedObj, &parent, true);
    } else {
        mozjs::ValueReader(cx, args.rval())
            .fromBSONElement(decryptedObj.firstElement(), parent, true);
    }
}

boost::optional<EncryptedFieldConfig> EncryptedDBClientBase::getEncryptedFieldConfig(
    const NamespaceString& nss) {
    // There are no guarantees that retrieving collection information with secondary reads enabled
    // will reflect its own creation in a sharded cluster. To avoid a NamespaceNotFound error when
    // using a connection from the router, we should fetch the information from the primary node of
    // the replica set.
    bool useSecondaryReads = !_conn->isMongos();
    auto collsList =
        _conn->getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()), useSecondaryReads);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Namespace not found: " << nss.toStringForErrorMsg(),
            !collsList.empty());
    auto info = collsList.front();
    auto opts = info.getField("options");
    if (opts.eoo() || !opts.isABSONObj()) {
        return boost::none;
    }
    // BSONObj must outlive BSONElement. See BSONElement, BSONObj::getField().
    auto optsObj = opts.Obj();
    auto efc = optsObj.getField("encryptedFields");
    if (efc.eoo() || !efc.isABSONObj()) {
        return boost::none;
    }
    return EncryptedFieldConfig::parse(efc.Obj(), IDLParserContext("encryptedFields"));
}

std::tuple<NamespaceString, BSONObj> validateStructuredEncryptionParams(JSContext* cx,
                                                                        JS::CallArgs args,
                                                                        StringData cmdName) {
    if ((args.length() < 1) || (args.length() > 2)) {
        uasserted(ErrorCodes::BadValue, str::stream() << cmdName << " requires 1 or 2 args");
    }
    uassert(ErrorCodes::BadValue,
            fmt::format("1st param to {} has to be a string", cmdName),
            args.get(0).isString());

    std::string fullName = mozjs::ValueWriter(cx, args.get(0)).toString();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(fullName);

    uassert(
        ErrorCodes::BadValue, str::stream() << "Invalid namespace: " << fullName, nss.isValid());

    BSONObj extra;
    if (args.length() >= 2) {
        uassert(ErrorCodes::BadValue,
                fmt::format("2nd param to {} has to be an object", cmdName),
                args.get(1).isObject());
        extra = mozjs::ValueWriter(cx, args.get(1)).toBSON();
    }

    return std::tuple(nss, extra);
}

void EncryptedDBClientBase::compact(JSContext* cx, JS::CallArgs args) {
    auto [nss, extra] = validateStructuredEncryptionParams(cx, args, compactCmdName);

    BSONObjBuilder builder;
    auto efc = getEncryptedFieldConfig(nss);

    builder.append("compactStructuredEncryptionData", nss.coll());

    if (extra["compactionTokens"_sd].eoo()) {
        builder.append("compactionTokens",
                       efc ? FLEClientCrypto::generateCompactionTokens(*efc, this) : BSONObj());
    }

    if (efc && extra["encryptionInformation"_sd].eoo() &&
        hasQueryTypeMatching(*efc, [](QueryTypeEnum type) {
            return type == QueryTypeEnum::Range || isFLE2TextQueryType(type);
        })) {
        EncryptionInformation ei;
        ei.setSchema(BSON(nss.serializeWithoutTenantPrefix_UNSAFE() << efc->toBSON()));
        builder.append("encryptionInformation"_sd, ei.toBSON());
    }

    for (const auto& [fieldName, _] : extra) {
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid field '{}' for CompactStructuredEncryptionData command",
                            fieldName),
                fieldName == "compactionTokens" || fieldName == "encryptionInformation" ||
                    fieldName == "anchorPaddingFactor");
    }
    builder.appendElements(extra);

    BSONObj reply;
    runCommand(nss.dbName(), builder.obj(), reply, 0);
    reply = reply.getOwned();
    mozjs::ValueReader(cx, args.rval()).fromBSON(reply, nullptr, false);
}

void EncryptedDBClientBase::cleanup(JSContext* cx, JS::CallArgs args) {
    auto [nss, extra] = validateStructuredEncryptionParams(cx, args, cleanupCmdName);

    BSONObjBuilder builder;
    auto efc = getEncryptedFieldConfig(nss);

    builder.append("cleanupStructuredEncryptionData", nss.coll());

    if (extra["cleanupTokens"_sd].eoo()) {
        builder.append("cleanupTokens",
                       efc ? FLEClientCrypto::generateCompactionTokens(*efc, this) : BSONObj());
    }

    for (const auto& [fieldName, _] : extra) {
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid field '{}' for CleanupStructuredEncryptionData command",
                            fieldName),
                fieldName == "cleanupTokens");
    }

    builder.appendElements(extra);

    BSONObj reply;
    runCommand(nss.dbName(), builder.obj(), reply, 0);
    reply = reply.getOwned();
    mozjs::ValueReader(cx, args.rval()).fromBSON(reply, nullptr, false);
}

void EncryptedDBClientBase::_getCompactionTokens(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue,
            "_getCompactionTokens() expects exactly one arg of type String",
            (args.length() == 1) && args.get(0).isString());

    std::string nssStr = mozjs::ValueWriter(cx, args.get(0)).toString();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(nssStr);
    auto efc = getEncryptedFieldConfig(nss);
    auto reply = efc ? FLEClientCrypto::generateCompactionTokens(*efc, this) : BSONObj();
    mozjs::ValueReader(cx, args.rval()).fromBSON(reply, nullptr, false);
}

void EncryptedDBClientBase::trace(JSTracer* trc) {
    JS::TraceEdge(trc, &_collection, "collection object");
}

void EncryptedDBClientBase::getEncryptionOptions(JSContext* cx, JS::CallArgs args) {
    mozjs::ValueReader(cx, args.rval()).fromBSON(_encryptionOptions.toBSON(), nullptr, false);
}

const ClientSideFLEOptions& EncryptedDBClientBase::getEncryptionOptions() const {
    return _encryptionOptions;
}

JS::Value EncryptedDBClientBase::getCollection() const {
    return _collection.get();
}

JS::Value EncryptedDBClientBase::getKeyVaultMongo() const {
    JS::RootedValue mongoRooted(_cx);
    JS::RootedObject collectionRooted(_cx, &_collection.get().toObject());
    JS_GetProperty(_cx, collectionRooted, "_mongo", &mongoRooted);
    if (!mongoRooted.isObject()) {
        uasserted(ErrorCodes::BadValue, "Collection object is incomplete.");
    }
    return mongoRooted.get();
}

std::unique_ptr<DBClientCursor> EncryptedDBClientBase::find(FindCommandRequest findRequest,
                                                            const ReadPreferenceSetting& readPref,
                                                            ExhaustMode exhaustMode) {
    return _conn->find(std::move(findRequest), readPref, exhaustMode);
}

bool EncryptedDBClientBase::isFailed() const {
    return _conn->isFailed();
}

bool EncryptedDBClientBase::isStillConnected() {
    return _conn->isStillConnected();
}

ConnectionString::ConnectionType EncryptedDBClientBase::type() const {
    return _conn->type();
}

double EncryptedDBClientBase::getSoTimeout() const {
    return _conn->getSoTimeout();
}

bool EncryptedDBClientBase::isReplicaSetMember() const {
    return _conn->isReplicaSetMember();
}

bool EncryptedDBClientBase::isMongos() const {
    return _conn->isMongos();
}

FLEEncryptionFrame EncryptedDBClientBase::createEncryptionFrame(std::shared_ptr<SymmetricKey> key,
                                                                FleAlgorithmInt algorithm,
                                                                UUID uuid,
                                                                BSONType type,
                                                                ConstDataRange plaintext) {

    auto cipherLength = crypto::aeadCipherOutputLength(plaintext.length());
    FLEEncryptionFrame dataframe(key, algorithm, uuid, type, plaintext, cipherLength);
    uassertStatusOK(crypto::aeadEncryptDataFrame(dataframe));
    return dataframe;
}

FLEDecryptionFrame EncryptedDBClientBase::createDecryptionFrame(ConstDataRange data) {
    auto frame = FLEDecryptionFrame(data);
    auto key = getDataKey(frame.getUUID());
    frame.setKey(key);
    uassertStatusOK(crypto::aeadDecryptDataFrame(frame));
    return frame;
}

NamespaceString EncryptedDBClientBase::getCollectionNS() {
    JS::RootedValue fullNameRooted(_cx);
    JS::RootedObject collectionRooted(_cx, &_collection.get().toObject());
    JS_GetProperty(_cx, collectionRooted, "_fullName", &fullNameRooted);
    if (!fullNameRooted.isString()) {
        uasserted(ErrorCodes::BadValue, "Collection object is incomplete.");
    }
    std::string fullName = mozjs::ValueWriter(_cx, fullNameRooted).toString();
    NamespaceString fullNameNS = NamespaceString::createNamespaceString_forTest(fullName);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Invalid namespace: " << fullName,
            fullNameNS.isValid());
    return fullNameNS;
}

std::vector<uint8_t> EncryptedDBClientBase::getBinDataArg(
    mozjs::MozJSImplScope* scope, JSContext* cx, JS::CallArgs args, int index, BinDataType type) {
    if (!args.get(index).isObject() ||
        !scope->getProto<mozjs::BinDataInfo>().instanceOf(args.get(index))) {
        uasserted(ErrorCodes::BadValue, "First parameter must be a BinData object");
    }

    mozjs::ObjectWrapper o(cx, args.get(index));

    auto binType = BinDataType(static_cast<int>(o.getNumber(mozjs::InternedString::type)));
    uassert(ErrorCodes::BadValue,
            str::stream() << "Incorrect bindata type, expected" << typeName(type) << " but got "
                          << typeName(binType),
            binType == type);
    auto str = JS::GetMaybePtrFromReservedSlot<std::string>(args.get(index).toObjectOrNull(),
                                                            mozjs::BinDataInfo::BinDataStringSlot);
    uassert(ErrorCodes::BadValue, "Cannot call getter on BinData prototype", str);
    std::string string = base64::decode(*str);
    return std::vector<uint8_t>(string.data(), string.data() + string.length());
}

std::shared_ptr<SymmetricKey> EncryptedDBClientBase::getDataKey(const UUID& uuid) {
    auto ts_new = Date_t::now();

    if (_datakeyCache.hasKey(uuid)) {
        auto [key, ts] = _datakeyCache.find(uuid)->second;
        if (ts_new - ts < kCacheInvalidationTime) {
            return key;
        } else {
            _datakeyCache.erase(uuid);
        }
    }
    auto key = getDataKeyFromDisk(uuid);
    _datakeyCache.add(uuid, std::make_pair(key, ts_new));
    return key;
}

DBClientBase* EncryptedDBClientBase::getRawConnection() {
    return _conn.get();
}

BSONObj EncryptedDBClientBase::doFindOne(OpMsgRequest& req) {
    // We directly "call" the server so we have fine grained control over how ValidatedTenancyScope
    // is handled.
    Client* client = &cc();

    auto msg = req.serialize();
    auto reply = _conn->call(msg);

    OpMsg opReply = OpMsg::parse(reply, client);
    BSONObj ownedResponse = opReply.body.getOwned();
    CursorResponse cp = uassertStatusOK(CursorResponse::parseFromBSON(ownedResponse));

    uassert(ErrorCodes::BadValue,
            "EncryptedDBClientBase findOne found many documents",
            cp.getBatch().size() <= 1);

    return cp.getBatch().size() == 1 ? cp.getBatch()[0] : BSONObj();
}

BSONObj EncryptedDBClientBase::getEncryptedKey(const UUID& uuid) {
    NamespaceString fullNameNS = getCollectionNS();
    FindCommandRequest findCmd{fullNameNS};
    findCmd.setFilter(BSON("_id" << uuid));
    findCmd.setReadConcern(repl::ReadConcernArgs::kMajority);
    findCmd.setReadPreference(ReadPreferenceSetting{ReadPreference::PrimaryPreferred});

    Client* client = &cc();
    auto opCtx = client->getOperationContext();

    boost::optional<auth::ValidatedTenancyScope> vts;
    if (opCtx) {
        vts = auth::ValidatedTenancyScope::get(opCtx);
    }

    OpMsgRequest req = OpMsgRequestBuilder::create(vts, fullNameNS.dbName(), findCmd.toBSON());

    BSONObj dataKeyObj = doFindOne(req);

    if (dataKeyObj.isEmpty()) {
        uasserted(ErrorCodes::BadValue,
                  fmt::format("Unable to find key ID {} from {} on node {}",
                              uuid.toString(),
                              fullNameNS.toStringForErrorMsg(),
                              _conn->getServerAddress()));
    }

    auto keyStoreRecord = KeyStoreRecord::parse(dataKeyObj, IDLParserContext("root"));

    BSONElement elem = dataKeyObj.getField("keyMaterial"_sd);
    uassert(ErrorCodes::BadValue,
            fmt::format("Key ID {} is not a generic BinData type", uuid.toString()),
            elem.isBinData(BinDataType::BinDataGeneral));

    uassert(ErrorCodes::BadValue,
            fmt::format("Key ID {} has invalid version - must be either 0 or undefined",
                        uuid.toString()),
            keyStoreRecord.getVersion() == 0);

    auto dataKey = keyStoreRecord.getKeyMaterial();
    uassert(ErrorCodes::BadValue,
            fmt::format("Key ID {} has invalid length", uuid.toString()),
            dataKey.length() != 0);

    return keyStoreRecord.toBSON();
}

SecureVector<uint8_t> EncryptedDBClientBase::getKeyMaterialFromDisk(const UUID& uuid) {
    auto rawKey = getEncryptedKey(uuid);
    auto keyStoreRecord = KeyStoreRecord::parse(rawKey, IDLParserContext("root"));

    auto dataKey = keyStoreRecord.getKeyMaterial();

    std::unique_ptr<KMSService> kmsService = KMSServiceController::createFromDisk(
        _encryptionOptions.getKmsProviders().toBSON(), keyStoreRecord.getMasterKey());
    SecureVector<uint8_t> decryptedKey =
        kmsService->decrypt(dataKey, keyStoreRecord.getMasterKey());
    return decryptedKey;
}

std::shared_ptr<SymmetricKey> EncryptedDBClientBase::getDataKeyFromDisk(const UUID& uuid) {
    auto decryptedKey = getKeyMaterialFromDisk(uuid);
    return std::make_shared<SymmetricKey>(
        std::move(decryptedKey), crypto::aesAlgorithm, "kms_encryption");
}

KeyMaterial EncryptedDBClientBase::getKey(const UUID& uuid) {
    auto decryptedKey = getKeyMaterialFromDisk(uuid);

    KeyMaterial km;
    km->resize(decryptedKey->size());
    std::copy(decryptedKey->data(), decryptedKey->data() + decryptedKey->size(), km->data());
    return km;
}

SymmetricKey& EncryptedDBClientBase::getKMSLocalKey() {
    if (!_localKey.has_value()) {
        std::unique_ptr<KMSService> kmsService = KMSServiceController::createFromDisk(
            _encryptionOptions.getKmsProviders().toBSON(), BSON("provider" << "local"));
        _localKey = std::move(kmsService->getMasterKey());
    }

    return _localKey.get();
}

#ifdef MONGO_CONFIG_SSL
const SSLConfiguration* EncryptedDBClientBase::getSSLConfiguration() {
    return _conn->getSSLConfiguration();
}

bool EncryptedDBClientBase::isTLS() {
    return _conn->isTLS();
}
#endif

namespace {

/**
 * Constructs a collection object from a namespace, passed in to the nsString parameter.
 * The client is the connection to a database in which you want to create the collection.
 * The collection parameter gets set to a javascript collection object.
 */
void createCollectionObject(JSContext* cx,
                            JS::HandleValue client,
                            StringData nsString,
                            JS::MutableHandleValue collection) {
    invariant(!client.isNull() && !client.isUndefined());

    auto ns = NamespaceString::createNamespaceString_forTest(nsString);
    uassert(ErrorCodes::BadValue,
            "Invalid keystore namespace.",
            ns.isValid() && NamespaceString::validCollectionName(ns.coll()));

    auto scope = mozjs::getScope(cx);

    // The collection object requires a database object to be constructed as well.
    JS::RootedValue databaseRV(cx);
    JS::RootedValueArray<2> databaseArgs(cx);

    databaseArgs[0].setObject(client.toObject());
    mozjs::ValueReader(cx, databaseArgs[1]).fromStringData(ns.dbName().toString_forTest());
    scope->getProto<mozjs::DBInfo>().newInstance(databaseArgs, &databaseRV);

    invariant(databaseRV.isObject());
    auto databaseObj = databaseRV.toObjectOrNull();

    JS::RootedValueArray<4> collectionArgs(cx);
    collectionArgs[0].setObject(client.toObject());
    collectionArgs[1].setObject(*databaseObj);
    mozjs::ValueReader(cx, collectionArgs[2]).fromStringData(ns.coll());
    mozjs::ValueReader(cx, collectionArgs[3]).fromStringData(ns.toString_forTest());

    scope->getProto<mozjs::DBCollectionInfo>().newInstance(collectionArgs, collection);
}

// The parameters required to start FLE on the shell. The current connection is passed in as a
// parameter to create the keyvault collection object if one is not provided.
std::shared_ptr<DBClientBase> createEncryptedDBClientBase(std::shared_ptr<DBClientBase> conn,
                                                          JS::HandleValue arg,
                                                          JS::HandleObject mongoConnection,
                                                          JSContext* cx) {

    uassert(
        31038, "Invalid Client Side Encryption parameters.", arg.isObject() || arg.isUndefined());

    static constexpr auto keyVaultClientFieldId = "keyVaultClient";

    if (!arg.isObject()) {
        return nullptr;
    }

    ClientSideFLEOptions encryptionOptions;
    JS::RootedValue client(cx);
    JS::RootedValue collection(cx);

    {
        uassert(ErrorCodes::BadValue,
                "Collection object must be passed to Field Level Encryption Options",
                arg.isObject());

        const BSONObj obj = mozjs::ValueWriter(cx, arg).toBSON();
        encryptionOptions = encryptionOptions.parse(obj, IDLParserContext("root"));

        // IDL does not perform a deep copy of BSONObjs when parsing, so we must get an
        // owned copy of the schemaMap.
        if (encryptionOptions.getSchemaMap()) {
            encryptionOptions.setSchemaMap(encryptionOptions.getSchemaMap().value().getOwned());
        }

        // This logic tries to extract the client from the args. If the connection object is defined
        // in the ClientSideFLEOptions struct, then the client will extract it and set itself to be
        // that. Else, the client will default to the implicit connection.
        JS::RootedObject handleObject(cx, &arg.toObject());
        JS_GetProperty(cx, handleObject, keyVaultClientFieldId, &client);
        if (client.isNull() || client.isUndefined()) {
            client.setObjectOrNull(mongoConnection.get());
        }
    }

    createCollectionObject(cx, client, encryptionOptions.getKeyVaultNamespace(), &collection);

    if (implicitEncryptedDBClientCallback != nullptr) {
        return implicitEncryptedDBClientCallback(
            std::move(conn), encryptionOptions, collection, cx);
    }

    return std::make_shared<EncryptedDBClientBase>(
        std::move(conn), encryptionOptions, collection, cx);
}

std::shared_ptr<DBClientBase> createEncryptedDBClientBaseFromExisting(
    std::shared_ptr<DBClientBase> encConn, std::shared_ptr<DBClientBase> rawConn, JSContext* cx) {
    auto encConnPtr = dynamic_cast<EncryptedDBClientBase*>(encConn.get());
    uassert(ErrorCodes::BadValue, "Connection is not a valid encrypted connection", encConnPtr);

    JS::RootedValue encOptsRV(cx);
    JS::RootedObject kvMongoRO(cx);
    mozjs::ValueReader(cx, &encOptsRV)
        .fromBSON(encConnPtr->getEncryptionOptions().toBSON(), nullptr, false);
    kvMongoRO.set(encConnPtr->getKeyVaultMongo().toObjectOrNull());

    return createEncryptedDBClientBase(rawConn, encOptsRV, kvMongoRO, cx);
}

DBClientBase* getNestedConnection(DBClientBase* conn) {
    auto* encryptedConn = dynamic_cast<EncryptedDBClientBase*>(conn);
    if (!encryptedConn) {
        return nullptr;
    }
    return encryptedConn->getRawConnection();
}

MONGO_INITIALIZER(setCallbacksForEncryptedDBClientBase)(InitializerContext*) {
    mongo::mozjs::setEncryptedDBClientCallbacks(
        createEncryptedDBClientBase, createEncryptedDBClientBaseFromExisting, getNestedConnection);
}

}  // namespace
}  // namespace mongo

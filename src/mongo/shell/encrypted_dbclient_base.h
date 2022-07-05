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

#include "mongo/platform/basic.h"

#include "mongo/base/data_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/config.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/object_check.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/scripting/mozjs/bindata.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/maxkey.h"
#include "mongo/scripting/mozjs/minkey.h"
#include "mongo/scripting/mozjs/mongo.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/shell/encrypted_shell_options.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/shell/shell_options.h"
#include "mongo/util/lru_cache.h"

namespace mongo {

constexpr std::size_t kEncryptedDBCacheSize = 50;

constexpr uint8_t kIntentToEncryptBit = 0x00;
constexpr uint8_t kDeterministicEncryptionBit = 0x01;
constexpr uint8_t kRandomEncryptionBit = 0x02;

static constexpr auto kExplain = "explain"_sd;

constexpr std::array<StringData, 14> kEncryptedCommands = {"aggregate"_sd,
                                                           "count"_sd,
                                                           "delete"_sd,
                                                           "distinct"_sd,
                                                           kExplain,
                                                           "find"_sd,
                                                           "findandmodify"_sd,
                                                           "findAndModify"_sd,
                                                           "getMore"_sd,
                                                           "insert"_sd,
                                                           "update"_sd,
                                                           "create"_sd,
                                                           "createIndexes"_sd,
                                                           "collMod"_sd};

class EncryptedDBClientBase : public DBClientBase,
                              public mozjs::EncryptionCallbacks,
                              public FLEKeyVault {
public:
    using DBClientBase::find;

    EncryptedDBClientBase(std::unique_ptr<DBClientBase> conn,
                          ClientSideFLEOptions encryptionOptions,
                          JS::HandleValue collection,
                          JSContext* cx);

    std::string getServerAddress() const final;

    void say(Message& toSend, bool isRetry, std::string* actualServer) final;

    using DBClientBase::runCommandWithTarget;
    virtual std::pair<rpc::UniqueReply, DBClientBase*> runCommandWithTarget(
        OpMsgRequest request) override;
    std::string toString() const final;

    int getMinWireVersion() final;

    int getMaxWireVersion() final;

    using EncryptionCallbacks::generateDataKey;
    void generateDataKey(JSContext* cx, JS::CallArgs args) final;

    using EncryptionCallbacks::getDataKeyCollection;
    void getDataKeyCollection(JSContext* cx, JS::CallArgs args) final;

    using EncryptionCallbacks::encrypt;
    void encrypt(mozjs::MozJSImplScope* scope, JSContext* cx, JS::CallArgs args) final;

    using EncryptionCallbacks::decrypt;
    void decrypt(mozjs::MozJSImplScope* scope, JSContext* cx, JS::CallArgs args) final;

    using EncryptionCallbacks::compact;
    void compact(JSContext* cx, JS::CallArgs args) final;

    using EncryptionCallbacks::trace;
    void trace(JSTracer* trc) final;

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& readPref,
                                         ExhaustMode exhaustMode) final;

    bool isFailed() const final;

    bool isStillConnected() final;

    ConnectionString::ConnectionType type() const final;

    double getSoTimeout() const final;

    bool isReplicaSetMember() const final;

    bool isMongos() const final;

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override;

    bool isTLS() final;
#endif

    KeyMaterial getKey(const UUID& uuid) final;

protected:
    BSONObj _decryptResponsePayload(BSONObj& reply, StringData databaseName, bool isFLE2);

    std::pair<rpc::UniqueReply, DBClientBase*> processResponseFLE1(rpc::UniqueReply result,
                                                                   StringData databaseName);

    std::pair<rpc::UniqueReply, DBClientBase*> processResponseFLE2(rpc::UniqueReply result,
                                                                   StringData databaseName);

    std::pair<rpc::UniqueReply, DBClientBase*> prepareReply(rpc::UniqueReply result,
                                                            StringData databaseName,
                                                            BSONObj decryptedDoc);


    BSONObj encryptDecryptCommand(const BSONObj& object, bool encrypt, StringData databaseName);

    JS::Value getCollection() const;

    BSONObj validateBSONElement(ConstDataRange out, uint8_t bsonType);

    NamespaceString getCollectionNS();

    std::shared_ptr<SymmetricKey> getDataKey(const UUID& uuid);

    FLEEncryptionFrame createEncryptionFrame(std::shared_ptr<SymmetricKey> key,
                                             FleAlgorithmInt algorithm,
                                             UUID uuid,
                                             BSONType type,
                                             ConstDataRange plaintext);

    FLEDecryptionFrame createDecryptionFrame(ConstDataRange data);

private:
    void _call(Message& toSend, Message& response, std::string* actualServer) final;

    virtual void encryptMarking(const BSONObj& elem, BSONObjBuilder* builder, StringData elemName);

    void decryptPayload(ConstDataRange data, BSONObjBuilder* builder, StringData elemName);

    std::vector<uint8_t> getBinDataArg(mozjs::MozJSImplScope* scope,
                                       JSContext* cx,
                                       JS::CallArgs args,
                                       int index,
                                       BinDataType type);

    std::shared_ptr<SymmetricKey> getDataKeyFromDisk(const UUID& uuid);
    SecureVector<uint8_t> getKeyMaterialFromDisk(const UUID& uuid);

    boost::optional<EncryptedFieldConfig> getEncryptedFieldConfig(const NamespaceString& nss);

protected:
    std::unique_ptr<DBClientBase> _conn;
    ClientSideFLEOptions _encryptionOptions;

private:
    LRUCache<UUID, std::pair<std::shared_ptr<SymmetricKey>, Date_t>, UUID::Hash> _datakeyCache{
        kEncryptedDBCacheSize};
    JS::Heap<JS::Value> _collection;
    JSContext* _cx;
};

using ImplicitEncryptedDBClientCallback =
    std::unique_ptr<DBClientBase>(std::unique_ptr<DBClientBase> conn,
                                  ClientSideFLEOptions encryptionOptions,
                                  JS::HandleValue collection,
                                  JSContext* cx);
void setImplicitEncryptedDBClientCallback(ImplicitEncryptedDBClientCallback* callback);


}  // namespace mongo

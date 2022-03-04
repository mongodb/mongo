/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <algorithm>
#include <array>
#include <string>
#include <third_party/murmurhash3/MurmurHash3.h>
#include <unordered_map>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
class FLEQueryTestImpl : public FLEQueryInterface {
public:
    FLEQueryTestImpl(OperationContext* opCtx, repl::StorageInterface* storage)
        : _opCtx(opCtx), _storage(storage) {}
    ~FLEQueryTestImpl() = default;

    BSONObj getById(const NamespaceString& nss, PrfBlock block);

    uint64_t countDocuments(const NamespaceString& nss);

    void insertDocument(const NamespaceString& nss, BSONObj obj, bool translateDuplicateKey);

    BSONObj deleteWithPreimage(const NamespaceString& nss, BSONObj query);

private:
    OperationContext* _opCtx;
    repl::StorageInterface* _storage;
};

BSONObj FLEQueryTestImpl::getById(const NamespaceString& nss, PrfBlock block) {
    auto obj = BSON("_id" << BSONBinData(block.data(), block.size(), BinDataGeneral));
    auto swDoc = _storage->findById(_opCtx, nss, obj.firstElement());
    if (swDoc.getStatus() == ErrorCodes::NoSuchKey) {
        return BSONObj();
    }

    return uassertStatusOK(swDoc);
}

uint64_t FLEQueryTestImpl::countDocuments(const NamespaceString& nss) {
    return uassertStatusOK(_storage->getCollectionCount(_opCtx, nss));
}

void FLEQueryTestImpl::insertDocument(const NamespaceString& nss,
                                      BSONObj obj,
                                      bool translateDuplicateKey) {
    repl::TimestampedBSONObj tb;
    tb.obj = obj;

    auto status = _storage->insertDocument(_opCtx, nss, tb, 0);

    uassertStatusOK(status);
}

FLEIndexKey indexKey(KeyMaterial{0x6e, 0xda, 0x88, 0xc8, 0x49, 0x6e, 0xc9, 0x90, 0xf5, 0xd5, 0x51,
                                 0x8d, 0xd2, 0xad, 0x6f, 0x3d, 0x9c, 0x33, 0xb6, 0x05, 0x59, 0x04,
                                 0xb1, 0x20, 0xf1, 0x2d, 0xe8, 0x29, 0x11, 0xfb, 0xd9, 0x33});

FLEUserKey userKey(KeyMaterial{0x1b, 0xd4, 0x32, 0xd4, 0xce, 0x54, 0x7d, 0xd7, 0xeb, 0xfb, 0x30,
                               0x9a, 0xea, 0xd6, 0xc6, 0x95, 0xfe, 0x53, 0xff, 0xe9, 0xc4, 0xb1,
                               0xc4, 0xf0, 0x6f, 0x36, 0x3c, 0xf0, 0x7b, 0x00, 0x28, 0xaf});

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

class TestKeyVault : public FLEKeyVault {
public:
    TestKeyVault() : _random(123456) {}

    KeyMaterial getKey(UUID uuid) override;

    uint64_t getCount() const {
        return _dynamicKeys.size();
    }

private:
    PseudoRandom _random;
    stdx::unordered_map<UUID, KeyMaterial, UUID::Hash> _dynamicKeys;
};

KeyMaterial TestKeyVault::getKey(UUID uuid) {
    if (uuid == indexKeyId) {
        return indexKey.data;
    } else if (uuid == userKeyId) {
        return userKey.data;
    } else {
        if (_dynamicKeys.find(uuid) != _dynamicKeys.end()) {
            return _dynamicKeys[uuid];
        }

        KeyMaterial material;
        _random.fill(&material, sizeof(material));
        _dynamicKeys.insert({uuid, material});
        return material;
    }
}

UUID fieldNameToUUID(StringData field) {
    std::array<uint8_t, UUID::kNumBytes> buf;

    MurmurHash3_x86_128(field.rawData(), field.size(), 123456, buf.data());

    return UUID::fromCDR(buf);
}

std::string fieldNameFromInt(uint64_t i) {
    return "field" + std::to_string(i);
}

class FleCrudTest : public ServiceContextMongoDTest {
private:
    void setUp() final;
    void tearDown() final;

protected:
    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc);

    void doSingleInsert(int id, BSONElement element);
    void doSingleInsert(int id, BSONObj obj);

    using ValueGenerator = std::function<std::string(StringData fieldName, uint64_t row)>;

    void doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func);

    ESCTwiceDerivedTagToken getTestESCToken(BSONElement value);
    ESCTwiceDerivedTagToken getTestESCToken(BSONObj obj);
    ESCTwiceDerivedTagToken getTestESCToken(StringData name, StringData value);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    repl::StorageInterface* _storage{nullptr};

    std::unique_ptr<FLEQueryTestImpl> _queryImpl;

    TestKeyVault _keyVault;

    NamespaceString _edcNs{"test.edc"};
    NamespaceString _escNs{"test.esc"};
    NamespaceString _eccNs{"test.ecc"};
    NamespaceString _ecocNs{"test.ecoc"};
};

void FleCrudTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    _storage = repl::StorageInterface::get(service);

    _queryImpl = std::make_unique<FLEQueryTestImpl>(_opCtx.get(), _storage);

    createCollection(_edcNs);
    createCollection(_escNs);
    createCollection(_eccNs);
    createCollection(_ecocNs);
}

void FleCrudTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

void FleCrudTest::createCollection(const NamespaceString& ns) {
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(), NamespaceString(ns.db(), ns.coll()), collectionOptions);
    ASSERT_OK(statusCC);
}

ConstDataRange toCDR(BSONElement element) {
    return ConstDataRange(element.value(), element.value() + element.valuesize());
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONElement element) {
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, toCDR(element));
    auto escContentionToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);

    return FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escContentionToken);
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONObj obj) {
    return getTestESCToken(obj.firstElement());
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(StringData name, StringData value) {

    auto doc = BSON("i" << value);
    auto element = doc.firstElement();

    UUID keyId = fieldNameToUUID(name);

    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(keyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, toCDR(element));
    auto escContentionToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);

    return FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escContentionToken);
}

std::vector<char> FleCrudTest::generatePlaceholder(UUID keyId, BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(keyId);
    ep.setIndexKeyId(keyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

EncryptedFieldConfig getTestEncryptedFieldConfig() {

    constexpr auto schema = R"({
    "escCollection": "esc",
    "eccCollection": "ecc",
    "ecocCollection": "ecoc",
    "fields": [
        {
            "keyId":
                            {
                                "$uuid": "12345678-1234-9876-1234-123456789012"
                            }
                        ,
            "path": "encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality"}

        }
    ]
})";

    return EncryptedFieldConfig::parse(IDLParserErrorContext("root"), fromjson(schema));
}

void FleCrudTest::assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc) {
    ASSERT_EQ(_queryImpl->countDocuments(_edcNs), edc);
    ASSERT_EQ(_queryImpl->countDocuments(_escNs), esc);
    ASSERT_EQ(_queryImpl->countDocuments(_eccNs), ecc);
    ASSERT_EQ(_queryImpl->countDocuments(_ecocNs), ecoc);
}


// Auto generate key ids from field id
void FleCrudTest::doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func) {
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");

    for (uint64_t i = 0; i < fieldCount; i++) {
        auto name = fieldNameFromInt(i);
        auto value = func(name, id);
        auto doc = BSON("I" << value);
        UUID uuid = fieldNameToUUID(name);
        auto buf = generatePlaceholder(uuid, doc.firstElement());
        builder.appendBinData(name, buf.size(), BinDataType::Encrypt, buf.data());
    }

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig();

    processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, result);
}

// Use different keys for index and user
std::vector<char> generateSinglePlaceholder(BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

void FleCrudTest::doSingleInsert(int id, BSONElement element) {
    auto buf = generateSinglePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig();

    processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, result);
}

void FleCrudTest::doSingleInsert(int id, BSONObj obj) {
    doSingleInsert(id, obj.firstElement());
}

// Insert one document
TEST_F(FleCrudTest, InsertOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());
}

// Insert two documents with same values
TEST_F(FleCrudTest, InsertTwoSame) {

    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();
    doSingleInsert(1, element);
    doSingleInsert(2, element);

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());
    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 2))
                     .isEmpty());
}

// Insert two documents with different values
TEST_F(FleCrudTest, InsertTwoDifferent) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    doSingleInsert(2,
                   BSON("encrypted"
                        << "topsecret"));

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateId(getTestESCToken(BSON("encrypted"
                                                                              << "secret")),
                                                         1))
                     .isEmpty());
    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateId(getTestESCToken(BSON("encrypted"
                                                                              << "topsecret")),
                                                         1))
                     .isEmpty());
}

// Insert 1 document with 100 fields
TEST_F(FleCrudTest, Insert100Fields) {

    uint64_t fieldCount = 100;
    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return fieldName.toString();
    };
    doSingleWideInsert(1, fieldCount, valueGenerator);

    assertDocumentCounts(1, fieldCount, 0, fieldCount);

    for (uint64_t field = 0; field < fieldCount; field++) {
        auto fieldName = fieldNameFromInt(field);

        ASSERT_FALSE(
            _queryImpl
                ->getById(
                    _escNs,
                    ESCCollection::generateId(
                        getTestESCToken(fieldName, valueGenerator(fieldNameFromInt(field), 0)), 1))
                .isEmpty());
    }
}

// Insert 100 documents each with 20 fields with 7 distinct values per field
TEST_F(FleCrudTest, Insert20Fields50Rows) {

    uint64_t fieldCount = 20;
    uint64_t rowCount = 50;

    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return fieldName.toString() + std::to_string(row % 7);
    };


    for (uint64_t row = 0; row < rowCount; row++) {
        doSingleWideInsert(row, fieldCount, valueGenerator);
    }

    assertDocumentCounts(rowCount, rowCount * fieldCount, 0, rowCount * fieldCount);

    for (uint64_t row = 0; row < rowCount; row++) {
        for (uint64_t field = 0; field < fieldCount; field++) {
            auto fieldName = fieldNameFromInt(field);

            int count = (row / 7) + 1;

            ASSERT_FALSE(
                _queryImpl
                    ->getById(_escNs,
                              ESCCollection::generateId(
                                  getTestESCToken(fieldName,
                                                  valueGenerator(fieldNameFromInt(field), row)),
                                  count))
                    .isEmpty());
        }
    }
}

}  // namespace
}  // namespace mongo

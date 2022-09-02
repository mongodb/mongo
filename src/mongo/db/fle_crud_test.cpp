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

#include "mongo/base/error_codes.h"
#include "mongo/platform/basic.h"

#include <algorithm>
#include <array>
#include <string>
#include <third_party/murmurhash3/MurmurHash3.h>
#include <unordered_map>
#include <vector>

#include "boost/smart_ptr/intrusive_ptr.hpp"

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
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/fle_query_interface_mock.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
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

namespace fle {
size_t sizeArrayElementsMemory(size_t tagCount);
}

namespace {

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

const FLEIndexKey& getIndexKey() {
    static std::string indexVec = hexblob::decode(
        "f502e66502ff5d4559452ce928a0f08557a9853c4dfeacca77cff482434f0ca1251fbd60d200bc35f24309521b45ad781b2d3c4df788cacef3c0e7beca8170b6cfc514ecfcf27e217ed697ae65c08272886324def514b14369c7c60414e80f22"_sd);
    static FLEIndexKey indexKey(KeyMaterial(indexVec.begin(), indexVec.end()));
    return indexKey;
}

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "cbebdf05fe16099fef502a6d045c1cbb77d29d2fe19f51aec5079a81008305d8868358845d2e3ab38e4fa9cbffcd651a0fc07201d7c9ed9ca3279bfa7cd673ec37b362a0aaa92f95062405a999afd49e4b1f7f818f766c49715407011ac37fa9"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}

class TestKeyVault : public FLEKeyVault {
public:
    TestKeyVault() : _random(123456) {}

    KeyMaterial getKey(const UUID& uuid) override;

    uint64_t getCount() const {
        return _dynamicKeys.size();
    }

private:
    PseudoRandom _random;
    stdx::unordered_map<UUID, KeyMaterial, UUID::Hash> _dynamicKeys;
};

KeyMaterial TestKeyVault::getKey(const UUID& uuid) {
    if (uuid == indexKeyId) {
        return getIndexKey().data;
    } else if (uuid == userKeyId) {
        return getUserKey().data;
    } else {
        if (_dynamicKeys.find(uuid) != _dynamicKeys.end()) {
            return _dynamicKeys[uuid];
        }

        std::vector<uint8_t> materialVector(96);
        _random.fill(&materialVector[0], materialVector.size());
        KeyMaterial material(materialVector.begin(), materialVector.end());
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
protected:
    void setUp();
    void tearDown();

    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc);

    void testValidateEncryptedFieldInfo(BSONObj obj, bool bypassValidation);

    void testValidateTags(BSONObj obj);

    void doSingleInsert(int id,
                        BSONElement element,
                        Fle2AlgorithmInt alg = Fle2AlgorithmInt::kEquality,
                        bool bypassDocumentValidation = false);
    void doSingleInsert(int id, BSONObj obj, bool bypassDocumentValidation = false);

    void doSingleRangeInsert(int id, BSONElement element);

    void doSingleInsertWithContention(
        int id, BSONElement element, int64_t cm, uint64_t cf, EncryptedFieldConfig efc);
    void doSingleInsertWithContention(
        int id, BSONObj obj, int64_t cm, uint64_t cf, EncryptedFieldConfig efc);

    void doSingleDelete(int id, Fle2AlgorithmInt alg = Fle2AlgorithmInt::kEquality);

    void doSingleUpdate(int id, BSONElement element);
    void doSingleUpdate(int id, BSONObj obj);
    void doSingleUpdateWithUpdateDoc(int id, BSONObj update);
    void doSingleUpdateWithUpdateDoc(int id, const write_ops::UpdateModification& modification);

    void doFindAndModify(write_ops::FindAndModifyCommandRequest& request);

    using ValueGenerator = std::function<std::string(StringData fieldName, uint64_t row)>;

    void doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func);

    void validateDocument(int id, boost::optional<BSONObj> doc);

    ESCDerivedFromDataToken getTestESCDataToken(BSONObj obj);
    ECCDerivedFromDataToken getTestECCDataToken(BSONObj obj);
    EDCDerivedFromDataToken getTestEDCDataToken(BSONObj obj);

    ESCTwiceDerivedTagToken getTestESCToken(BSONElement value);
    ESCTwiceDerivedTagToken getTestESCToken(BSONObj obj);
    ESCTwiceDerivedTagToken getTestESCToken(StringData name, StringData value);

    ECCDerivedFromDataTokenAndContentionFactorToken getTestECCToken(BSONElement value);

    ECCDocument getECCDocument(ECCDerivedFromDataTokenAndContentionFactorToken token, int position);

    void assertECOCDocumentCountByField(StringData fieldName, uint64_t expect);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    repl::StorageInterface* _storage{nullptr};

    std::unique_ptr<FLEQueryInterfaceMock> _queryImpl;

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

    _queryImpl = std::make_unique<FLEQueryInterfaceMock>(_opCtx.get(), _storage);

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

ESCDerivedFromDataToken FleCrudTest::getTestESCDataToken(BSONObj obj) {
    auto element = obj.firstElement();
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);
    return FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken,
                                                                             toCDR(element));
}

ECCDerivedFromDataToken FleCrudTest::getTestECCDataToken(BSONObj obj) {
    auto element = obj.firstElement();
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(c1token);
    return FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken,
                                                                             toCDR(element));
}

EDCDerivedFromDataToken FleCrudTest::getTestEDCDataToken(BSONObj obj) {
    auto element = obj.firstElement();
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(c1token);
    return FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken,
                                                                             toCDR(element));
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

ECCDerivedFromDataTokenAndContentionFactorToken FleCrudTest::getTestECCToken(BSONElement element) {
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(c1token);
    auto eccDataToken =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, toCDR(element));
    return FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, 0);
}

ECCDocument FleCrudTest::getECCDocument(ECCDerivedFromDataTokenAndContentionFactorToken token,
                                        int position) {

    auto tag = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(token);
    auto value = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(token);

    BSONObj doc = _queryImpl->getById(_eccNs, ECCCollection::generateId(tag, position));
    ASSERT_FALSE(doc.isEmpty());

    return uassertStatusOK(ECCCollection::decryptDocument(value, doc));
}

void FleCrudTest::assertECOCDocumentCountByField(StringData fieldName, uint64_t expect) {
    auto query = BSON(EcocDocument::kFieldNameFieldName << fieldName);
    auto results = _queryImpl->findDocuments(_ecocNs, query);
    ASSERT_EQ(results.size(), expect);
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

EncryptedFieldConfig getTestEncryptedFieldConfig(
    Fle2AlgorithmInt alg = Fle2AlgorithmInt::kEquality) {

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

    constexpr auto rangeSchema = R"({
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
            "bsonType": "int",
            "queries": {"queryType": "range", "min": 0, "max": 15, "sparsity": 1}

        }
    ]
})";

    if (alg == Fle2AlgorithmInt::kEquality) {
        return EncryptedFieldConfig::parse(IDLParserContext("root"), fromjson(schema));
    }
    return EncryptedFieldConfig::parse(IDLParserContext("root"), fromjson(rangeSchema));
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

    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig();

    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}


void FleCrudTest::validateDocument(int id, boost::optional<BSONObj> doc) {

    auto doc1 = BSON("_id" << id);
    auto updatedDoc = _queryImpl->getById(_edcNs, doc1.firstElement());

    std::cout << "Updated Doc: " << updatedDoc << std::endl;

    auto efc = getTestEncryptedFieldConfig();
    FLEClientCrypto::validateDocument(updatedDoc, efc, &_keyVault);

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(updatedDoc, &_keyVault);

    if (doc.has_value()) {
        // Remove this so the round-trip is clean
        decryptedDoc = decryptedDoc.removeField(kSafeContent);

        ASSERT_BSONOBJ_EQ(doc.value(), decryptedDoc);
    }
}

BSONObj generateFLE2RangeInsertSpec(BSONElement value) {
    FLE2RangeInsertSpec spec;
    spec.setValue(value);

    auto lowerDoc = BSON("lb" << 0);
    spec.setLowerBound(lowerDoc.firstElement());
    auto upperDoc = BSON("ub" << 15);

    spec.setUpperBound(upperDoc.firstElement());
    auto specDoc = BSON("s" << spec.toBSON());

    return specDoc;
}

// Use different keys for index and user
std::vector<char> generateSinglePlaceholder(BSONElement value,
                                            Fle2AlgorithmInt alg = Fle2AlgorithmInt::kEquality,
                                            int64_t cm = 0) {
    FLE2EncryptionPlaceholder ep;

    // Has to be generated outside of if statements to root the
    // value until ep is finalized as an object.
    BSONObj temp = generateFLE2RangeInsertSpec(value);

    ep.setAlgorithm(alg);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);

    if (alg == Fle2AlgorithmInt::kRange) {
        ep.setValue(temp.firstElement());
        ep.setSparsity(1);
    } else {
        ep.setValue(value);
    }

    ep.setMaxContentionCounter(cm);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

void FleCrudTest::testValidateEncryptedFieldInfo(BSONObj obj, bool bypassValidation) {
    auto efc = getTestEncryptedFieldConfig();
    EDCServerCollection::validateEncryptedFieldInfo(obj, efc, bypassValidation);
}

void FleCrudTest::testValidateTags(BSONObj obj) {
    FLEClientCrypto::validateTagsArray(obj);
}

void FleCrudTest::doSingleInsert(int id,
                                 BSONElement element,
                                 Fle2AlgorithmInt alg,
                                 bool bypassDocumentValidation) {
    auto buf = generateSinglePlaceholder(element, alg);
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig(alg);

    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}

void FleCrudTest::doSingleInsert(int id, BSONObj obj, bool bypassDocumentValidation) {
    doSingleInsert(id, obj.firstElement());
}

void FleCrudTest::doSingleInsertWithContention(
    int id, BSONElement element, int64_t cm, uint64_t cf, EncryptedFieldConfig efc) {
    auto buf = generateSinglePlaceholder(element, Fle2AlgorithmInt::kEquality, cm);
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::transformPlaceholders(
        clientDoc, &_keyVault, [cf](const FLE2EncryptionPlaceholder&) { return cf; });

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}

void FleCrudTest::doSingleInsertWithContention(
    int id, BSONObj obj, int64_t cm, uint64_t cf, EncryptedFieldConfig efc) {
    doSingleInsertWithContention(id, obj.firstElement(), cm, cf, efc);
}

void FleCrudTest::doSingleUpdate(int id, BSONObj obj) {
    doSingleUpdate(id, obj.firstElement());
}

void FleCrudTest::doSingleUpdate(int id, BSONElement element) {
    auto buf = generateSinglePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set",
                   BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto clientDoc = builder.obj();
    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);

    doSingleUpdateWithUpdateDoc(id, result);
}

void FleCrudTest::doSingleUpdateWithUpdateDoc(int id, BSONObj update) {
    doSingleUpdateWithUpdateDoc(
        id,
        write_ops::UpdateModification(update, write_ops::UpdateModification::ModifierUpdateTag{}));
}

void FleCrudTest::doSingleUpdateWithUpdateDoc(int id,
                                              const write_ops::UpdateModification& modification) {

    auto efc = getTestEncryptedFieldConfig();
    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _edcNs, efc, &_keyVault);
    auto ei = EncryptionInformation::parse(IDLParserContext("test"), doc);

    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setU(modification);

    write_ops::UpdateCommandRequest updateRequest(_edcNs);
    updateRequest.setUpdates({entry});
    updateRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);


    std::unique_ptr<CollatorInterface> collator;
    auto expCtx = make_intrusive<ExpressionContext>(_opCtx.get(),
                                                    std::move(collator),
                                                    updateRequest.getNamespace(),
                                                    updateRequest.getLegacyRuntimeConstants(),
                                                    updateRequest.getLet());
    processUpdate(_queryImpl.get(), expCtx, updateRequest);
}

void FleCrudTest::doSingleDelete(int id, Fle2AlgorithmInt alg) {

    auto efc = getTestEncryptedFieldConfig(alg);

    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _edcNs, efc, &_keyVault);

    auto ei = EncryptionInformation::parse(IDLParserContext("test"), doc);

    write_ops::DeleteOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setMulti(false);

    write_ops::DeleteCommandRequest deleteRequest(_edcNs);
    deleteRequest.setDeletes({entry});
    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    std::unique_ptr<CollatorInterface> collator;
    auto expCtx = make_intrusive<ExpressionContext>(_opCtx.get(),
                                                    std::move(collator),
                                                    deleteRequest.getNamespace(),
                                                    deleteRequest.getLegacyRuntimeConstants(),
                                                    deleteRequest.getLet());

    processDelete(_queryImpl.get(), expCtx, deleteRequest);
}

void FleCrudTest::doFindAndModify(write_ops::FindAndModifyCommandRequest& request) {
    auto efc = getTestEncryptedFieldConfig();
    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _edcNs, efc, &_keyVault);
    auto ei = EncryptionInformation::parse(IDLParserContext("test"), doc);

    request.setEncryptionInformation(ei);

    std::unique_ptr<CollatorInterface> collator;
    auto expCtx = make_intrusive<ExpressionContext>(_opCtx.get(),
                                                    std::move(collator),
                                                    request.getNamespace(),
                                                    request.getLegacyRuntimeConstants(),
                                                    request.getLet());
    processFindAndModify(expCtx, _queryImpl.get(), request);
}

class CollectionReader : public FLEStateCollectionReader {
public:
    CollectionReader(std::string&& coll, FLEQueryInterfaceMock& queryImpl)
        : _coll(NamespaceString(coll)), _queryImpl(queryImpl) {}

    uint64_t getDocumentCount() const override {
        return _queryImpl.countDocuments(_coll);
    }

    BSONObj getById(PrfBlock block) const override {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        return _queryImpl.getById(_coll, doc.firstElement());
    }

private:
    NamespaceString _coll;
    FLEQueryInterfaceMock& _queryImpl;
};

class FleTagsTest : public FleCrudTest {
protected:
    void setUp() {
        FleCrudTest::setUp();
    }
    void tearDown() {
        FleCrudTest::tearDown();
    }
    std::vector<PrfBlock> readTagsWithContention(BSONObj obj, uint64_t contention = 0) {
        auto s = getTestESCDataToken(obj);
        auto c = getTestECCDataToken(obj);
        auto d = getTestEDCDataToken(obj);
        auto esc = CollectionReader("test.esc", *_queryImpl);
        auto ecc = CollectionReader("test.ecc", *_queryImpl);
        return mongo::fle::readTagsWithContention(esc, ecc, s, c, d, contention, 100, {});
    }
    std::vector<PrfBlock> readTags(BSONObj obj, uint64_t cm = 0) {
        auto s = getTestESCDataToken(obj);
        auto c = getTestECCDataToken(obj);
        auto d = getTestEDCDataToken(obj);
        auto esc = CollectionReader("test.esc", *_queryImpl);
        auto ecc = CollectionReader("test.ecc", *_queryImpl);
        return mongo::fle::readTags(esc, ecc, s, c, d, cm);
    }
};

// Insert one document
TEST_F(FleCrudTest, InsertOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);
    assertECOCDocumentCountByField("encrypted", 1);

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
    assertECOCDocumentCountByField("encrypted", 2);

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
    assertECOCDocumentCountByField("encrypted", 2);

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

        assertECOCDocumentCountByField(fieldName, 1);

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

            assertECOCDocumentCountByField(fieldName, rowCount);
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

#define ASSERT_ECC_DOC(assertElement, assertPosition, assertStart, assertEnd)            \
    {                                                                                    \
        auto _eccDoc = getECCDocument(getTestECCToken((assertElement)), assertPosition); \
        ASSERT(_eccDoc.valueType == ECCValueType::kNormal);                              \
        ASSERT_EQ(_eccDoc.start, assertStart);                                           \
        ASSERT_EQ(_eccDoc.end, assertEnd);                                               \
    }

// Insert and delete one document
TEST_F(FleCrudTest, InsertAndDeleteOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());

    doSingleDelete(1);

    assertDocumentCounts(0, 1, 1, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    getECCDocument(getTestECCToken(element), 1);
}

// Insert and delete one document
TEST_F(FleCrudTest, InsertAndDeleteOneRange) {
    auto doc = BSON("encrypted" << 5);
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 5, 0, 5);

    doSingleDelete(1, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(0, 5, 5, 10);
    assertECOCDocumentCountByField("encrypted", 10);
}

// Insert two documents, and delete both
TEST_F(FleCrudTest, InsertTwoSamAndDeleteTwo) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);
    doSingleInsert(2, element);

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());

    doSingleDelete(2);
    doSingleDelete(1);

    assertDocumentCounts(0, 2, 2, 4);
    assertECOCDocumentCountByField("encrypted", 4);
    ASSERT_ECC_DOC(element, 1, 2, 2);
    ASSERT_ECC_DOC(element, 2, 1, 1);
}

// Insert two documents with different values and delete them
TEST_F(FleCrudTest, InsertTwoDifferentAndDeleteTwo) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    doSingleInsert(2,
                   BSON("encrypted"
                        << "topsecret"));

    assertDocumentCounts(2, 2, 0, 2);

    doSingleDelete(2);
    doSingleDelete(1);

    assertDocumentCounts(0, 2, 2, 4);
    assertECOCDocumentCountByField("encrypted", 4);

    ASSERT_ECC_DOC(BSON("encrypted"
                        << "secret")
                       .firstElement(),
                   1,
                   1,
                   1);
    ASSERT_ECC_DOC(BSON("encrypted"
                        << "topsecret")
                       .firstElement(),
                   1,
                   1,
                   1);
}

// Insert one document but delete another document
TEST_F(FleCrudTest, InsertOneButDeleteAnother) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    assertDocumentCounts(1, 1, 0, 1);

    doSingleDelete(2);

    assertDocumentCounts(1, 1, 0, 1);
    assertECOCDocumentCountByField("encrypted", 1);
}

// Update one document
TEST_F(FleCrudTest, UpdateOne) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    doSingleUpdate(1,
                   BSON("encrypted"
                        << "top secret"));

    assertDocumentCounts(1, 2, 1, 3);
    assertECOCDocumentCountByField("encrypted", 3);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "top secret"));
}

// Update one document but to the same value
TEST_F(FleCrudTest, UpdateOneSameValue) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    doSingleUpdate(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 2, 1, 3);
    assertECOCDocumentCountByField("encrypted", 3);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "secret"));
}

// Update one document with replacement
TEST_F(FleCrudTest, UpdateOneReplace) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    auto replace = BSON("encrypted"
                        << "top secret");

    auto buf = generateSinglePlaceholder(replace.firstElement());

    auto replaceEP = BSON("plainText"
                          << "fake"
                          << "encrypted"
                          << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt));

    auto result = FLEClientCrypto::transformPlaceholders(replaceEP, &_keyVault);

    doSingleUpdateWithUpdateDoc(
        1, write_ops::UpdateModification(result, write_ops::UpdateModification::ReplacementTag{}));


    assertDocumentCounts(1, 2, 1, 3);
    assertECOCDocumentCountByField("encrypted", 3);

    validateDocument(1,
                     BSON("_id" << 1 << "plainText"
                                << "fake"
                                << "encrypted"
                                << "top secret"));
}

// Rename safeContent
TEST_F(FleCrudTest, RenameSafeContent) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$rename", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(doSingleUpdateWithUpdateDoc(1, result), DBException, 6371506);
}

// Mess with __safeContent__ and ensure the update errors
TEST_F(FleCrudTest, SetSafeContent) {
    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(doSingleUpdateWithUpdateDoc(1, result), DBException, 6666200);
}

// Test that EDCServerCollection::validateEncryptedFieldInfo checks that the
// safeContent cannot be present in the BSON obj.
TEST_F(FleCrudTest, testValidateEncryptedFieldConfig) {
    testValidateEncryptedFieldInfo(BSON(kSafeContent << "secret"), true);
    ASSERT_THROWS_CODE(testValidateEncryptedFieldInfo(BSON(kSafeContent << "secret"), false),
                       DBException,
                       6666200);
}

// Update one document via findAndModify
TEST_F(FleCrudTest, FindAndModify_UpdateOne) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    auto doc = BSON("encrypted"
                    << "top secret");
    auto element = doc.firstElement();
    auto buf = generateSinglePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set",
                   BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto clientDoc = builder.obj();
    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);


    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));
    doFindAndModify(req);

    assertDocumentCounts(1, 2, 1, 3);
    assertECOCDocumentCountByField("encrypted", 3);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "top secret"));
}

// Insert and delete one document via findAndModify
TEST_F(FleCrudTest, FindAndModify_InsertAndDeleteOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setRemove(true);
    doFindAndModify(req);

    assertDocumentCounts(0, 1, 1, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    getECCDocument(getTestECCToken(element), 1);
}

// Rename safeContent
TEST_F(FleCrudTest, FindAndModify_RenameSafeContent) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$rename", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));

    ASSERT_THROWS_CODE(doFindAndModify(req), DBException, 6371506);
}

TEST_F(FleCrudTest, validateTagsTest) {
    testValidateTags(BSON(kSafeContent << BSON_ARRAY(123)));
    ASSERT_THROWS_CODE(testValidateTags(BSON(kSafeContent << "foo")), DBException, 6371507);
}

// Mess with __safeContent__ and ensure the update errors
TEST_F(FleCrudTest, FindAndModify_SetSafeContent) {
    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));

    ASSERT_THROWS_CODE(doFindAndModify(req), DBException, 6666200);
}

BSONObj makeInsertUpdatePayload(StringData path, const UUID& uuid) {
    // Actual values don't matter for these tests (apart from indexKeyId).
    auto bson = FLE2InsertUpdatePayload({}, {}, {}, {}, uuid, BSONType::String, {}, {}).toBSON();
    std::vector<std::uint8_t> bindata;
    bindata.resize(bson.objsize() + 1);
    bindata[0] = static_cast<std::uint8_t>(EncryptedBinDataType::kFLE2InsertUpdatePayload);
    memcpy(bindata.data() + 1, bson.objdata(), bson.objsize());

    BSONObjBuilder bob;
    bob.appendBinData(path, bindata.size(), BinDataType::Encrypt, bindata.data());
    return bob.obj();
}

TEST(FleCrudTest, validateIndexKeyValid) {
    // This test assumes we have at least one field in EFC.
    auto fields = getTestEncryptedFieldConfig().getFields();
    ASSERT_GTE(fields.size(), 1);
    auto field = fields[0];

    auto validInsert = makeInsertUpdatePayload(field.getPath(), field.getKeyId());
    auto validPayload = EDCServerCollection::getEncryptedFieldInfo(validInsert);
    validateInsertUpdatePayloads(fields, validPayload);
}

TEST(FleCrudTest, validateIndexKeyInvalid) {
    // This test assumes we have at least one field in EFC.
    auto fields = getTestEncryptedFieldConfig().getFields();
    ASSERT_GTE(fields.size(), 1);
    auto field = fields[0];

    auto invalidInsert = makeInsertUpdatePayload(field.getPath(), UUID::gen());
    auto invalidPayload = EDCServerCollection::getEncryptedFieldInfo(invalidInsert);
    ASSERT_THROWS_WITH_CHECK(validateInsertUpdatePayloads(fields, invalidPayload),
                             DBException,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.what(),
                                                        str::stream()
                                                            << "Mismatched keyId for field '"
                                                            << field.getPath() << "'");
                             });
}

TEST_F(FleTagsTest, InsertOne) {
    auto doc = BSON("encrypted"
                    << "a");

    doSingleInsert(1, doc);

    ASSERT_EQ(1, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoSame) {
    auto doc = BSON("encrypted"
                    << "a");

    doSingleInsert(1, doc);
    doSingleInsert(2, doc);

    ASSERT_EQ(2, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoDifferent) {
    auto doc1 = BSON("encrypted"
                     << "a");
    auto doc2 = BSON("encrypted"
                     << "b");

    doSingleInsert(1, doc1);
    doSingleInsert(2, doc2);

    ASSERT_EQ(1, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, InsertAndDeleteOne) {
    auto doc = BSON("encrypted"
                    << "a");

    doSingleInsert(1, doc);
    doSingleDelete(1);

    ASSERT_EQ(0, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoSameAndDeleteOne) {
    auto doc = BSON("encrypted"
                    << "a");

    doSingleInsert(1, doc);
    doSingleInsert(2, doc);
    doSingleDelete(2);

    ASSERT_EQ(1, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoDifferentAndDeleteOne) {
    auto doc1 = BSON("encrypted"
                     << "a");
    auto doc2 = BSON("encrypted"
                     << "b");

    doSingleInsert(1, doc1);
    doSingleInsert(2, doc2);
    doSingleDelete(1);

    ASSERT_EQ(0, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, InsertAndUpdate) {
    auto doc1 = BSON("encrypted"
                     << "a");
    auto doc2 = BSON("encrypted"
                     << "b");

    doSingleInsert(1, doc1);
    doSingleUpdate(1, doc2);

    ASSERT_EQ(0, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, ContentionFactor) {
    auto efc = EncryptedFieldConfig::parse(IDLParserContext("root"), fromjson(R"({
        "escCollection": "esc",
        "eccCollection": "ecc",
        "ecocCollection": "ecoc",
        "fields": [{
            "keyId": { "$uuid": "12345678-1234-9876-1234-123456789012"},
            "path": "encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality", "contention": NumberLong(4)}
        }]
    })"));

    auto doc1 = BSON("encrypted"
                     << "a");
    auto doc2 = BSON("encrypted"
                     << "b");

    // Insert doc1 twice with a contention factor of 0 and once with a contention factor or 3.
    doSingleInsertWithContention(1, doc1, 4, 0, efc);
    doSingleInsertWithContention(4, doc1, 4, 3, efc);
    doSingleInsertWithContention(5, doc1, 4, 0, efc);

    // Insert doc2 once with a contention factor of 2 and once with a contention factor of 3.
    doSingleInsertWithContention(7, doc2, 4, 2, efc);
    doSingleInsertWithContention(8, doc2, 4, 3, efc);

    ASSERT_EQ(2, readTagsWithContention(doc1, 0).size());
    ASSERT_EQ(0, readTagsWithContention(doc2, 0).size());
    ASSERT_EQ(0, readTagsWithContention(doc1, 1).size());
    ASSERT_EQ(0, readTagsWithContention(doc2, 1).size());
    ASSERT_EQ(0, readTagsWithContention(doc1, 2).size());
    ASSERT_EQ(1, readTagsWithContention(doc2, 2).size());
    ASSERT_EQ(1, readTagsWithContention(doc1, 3).size());
    ASSERT_EQ(1, readTagsWithContention(doc2, 3).size());
    ASSERT_EQ(3, readTags(doc1, 4).size());
    ASSERT_EQ(2, readTags(doc2, 4).size());
}

TEST_F(FleTagsTest, MemoryLimit) {
    auto doc = BSON("encrypted"
                    << "a");

    const auto tagLimit = 10;

    // Set memory limit to 10 tags * 40 bytes per tag
    internalQueryFLERewriteMemoryLimit.store(tagLimit * 40);

    // Do 10 inserts
    for (auto i = 0; i < tagLimit; i++) {
        doSingleInsert(i, doc);
    }

    // readTags returns 10 tags which does not exceed memory limit.
    ASSERT_EQ(tagLimit, readTags(doc).size());

    doSingleInsert(10, doc);

    // readTags returns 11 tags which does exceed memory limit.
    ASSERT_THROWS_CODE(readTags(doc), DBException, ErrorCodes::FLEMaxTagLimitExceeded);

    doSingleDelete(5);

    // readTags returns 10 tags which does not exceed memory limit.
    ASSERT_EQ(tagLimit, readTags(doc).size());
}

TEST_F(FleTagsTest, SampleMemoryLimit) {

    struct S {
        size_t count;
        size_t size;
    };

    // clang-format off
    static const std::vector<S> testVector{
        { 0, 0 },
        { 1, 40 },
        { 5, 200 },
        { 10, 400 },
        { 11, 441 },
        { 98, 4008 },
        { 99, 4049 },
        { 100, 4090 },
        { 101, 4132 },
        { 219, 9088 },
        { 944, 39538 },
        { 998, 41806 },
        { 999, 41848 },
        { 1000, 41890 },
        { 1001, 41933 },
        { 1025, 42965 },
        { 1498, 63304 },
        { 2049, 86997 },
        { 2907, 123891 },
        { 5232, 223866 },
        { 5845, 250225 },
        { 7203, 308619 },
        { 7786, 333688 },
        { 8383, 359359 },
        { 9171, 393243 },
        { 9974, 427772 },
        { 9986, 428288 },
        { 9998, 428804 },
        { 9999, 428847 },
        { 10000, 428890 },
        { 10001, 428934 },
        { 10056, 431354 },
        { 10907, 468798 },
        { 12500, 538890 },
        { 13778, 595122 },
        { 13822, 597058 },
    };
    // clang-format on

    for (auto& xp : testVector) {
        auto size = mongo::fle::sizeArrayElementsMemory(xp.count);
        ASSERT_EQ(xp.size, size);
    }
}

}  // namespace
}  // namespace mongo

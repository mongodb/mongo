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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_range.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/fle_query_interface_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/murmur3.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "cbebdf05fe16099fef502a6d045c1cbb77d29d2fe19f51aec5079a81008305d8868358845d2e3ab38e4fa9cbffcd651a0fc07201d7c9ed9ca3279bfa7cd673ec37b362a0aaa92f95062405a999afd49e4b1f7f818f766c49715407011ac37fa9"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}

class TestKeyVault : public FLEKeyVault {
public:
    TestKeyVault() : _random(123456), _localKey(getLocalKey()) {}

    static SymmetricKey getLocalKey() {
        const uint8_t buf[]{0x32, 0x78, 0x34, 0x34, 0x2b, 0x78, 0x64, 0x75, 0x54, 0x61, 0x42, 0x42,
                            0x6b, 0x59, 0x31, 0x36, 0x45, 0x72, 0x35, 0x44, 0x75, 0x41, 0x44, 0x61,
                            0x67, 0x68, 0x76, 0x53, 0x34, 0x76, 0x77, 0x64, 0x6b, 0x67, 0x38, 0x74,
                            0x70, 0x50, 0x70, 0x33, 0x74, 0x7a, 0x36, 0x67, 0x56, 0x30, 0x31, 0x41,
                            0x31, 0x43, 0x77, 0x62, 0x44, 0x39, 0x69, 0x74, 0x51, 0x32, 0x48, 0x46,
                            0x44, 0x67, 0x50, 0x57, 0x4f, 0x70, 0x38, 0x65, 0x4d, 0x61, 0x43, 0x31,
                            0x4f, 0x69, 0x37, 0x36, 0x36, 0x4a, 0x7a, 0x58, 0x5a, 0x42, 0x64, 0x42,
                            0x64, 0x62, 0x64, 0x4d, 0x75, 0x72, 0x64, 0x6f, 0x6e, 0x4a, 0x31, 0x64};

        return SymmetricKey(&buf[0], sizeof(buf), 0, SymmetricKeyId("test"), 0);
    }

    KeyMaterial getKey(const UUID& uuid) override;
    BSONObj getEncryptedKey(const UUID& uuid) override;
    SymmetricKey& getKMSLocalKey() {
        return _localKey;
    }

    uint64_t getCount() const {
        return _dynamicKeys.size();
    }

private:
    PseudoRandom _random;
    stdx::unordered_map<UUID, KeyMaterial, UUID::Hash> _dynamicKeys;
    SymmetricKey _localKey;
};

KeyMaterial TestKeyVault::getKey(const UUID& uuid) {
    if (uuid == userKeyId) {
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

KeyStoreRecord makeKeyStoreRecord(UUID id, ConstDataRange cdr) {
    KeyStoreRecord ksr;
    ksr.set_id(id);
    auto now = Date_t::now();
    ksr.setCreationDate(now);
    ksr.setUpdateDate(now);
    ksr.setStatus(0);
    ksr.setKeyMaterial(cdr);

    LocalMasterKey mk;

    ksr.setMasterKey(mk.toBSON());
    return ksr;
}

BSONObj TestKeyVault::getEncryptedKey(const UUID& uuid) {
    auto dek = getKey(uuid);

    std::vector<std::uint8_t> ciphertext(crypto::aeadCipherOutputLength(dek->size()));

    uassertStatusOK(crypto::aeadEncryptLocalKMS(_localKey, *dek, {ciphertext}));

    return makeKeyStoreRecord(uuid, ciphertext).toBSON();
}

UUID fieldNameToUUID(StringData field) {
    std::array<char, UUID::kNumBytes> buf;
    murmur3(field, 123456 /*seed*/, buf);
    return UUID::fromCDR(buf);
}

class FleCompactTest : public ServiceContextMongoDTest {
public:
    struct ESCTestTokens {
        ESCDerivedFromDataTokenAndContentionFactorToken contentionDerived;
        ESCTwiceDerivedTagToken twiceDerivedTag;
        ESCTwiceDerivedValueToken twiceDerivedValue;

        AnchorPaddingRootToken anchorPaddingRoot;
        AnchorPaddingKeyToken anchorPaddingKey;
        AnchorPaddingValueToken anchorPaddingValue;
    };
    struct InsertionState {
        uint64_t count{0};
        uint64_t pos{0};
        uint64_t toInsertCount{0};
        std::vector<std::pair<uint64_t, uint64_t>> toDeleteRanges;
        std::map<uint64_t, int> insertedIds;
        std::string value;
    };

protected:
    void setUp();
    void tearDown();

    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecoc);

    void assertESCNonAnchorDocument(BSONObj obj, bool exists, uint64_t cpos);
    void assertESCAnchorDocument(
        BSONObj obj, bool exists, uint64_t apos, uint64_t cpos, bool nullAnchor = false);
    void assertESCNullAnchorDocument(BSONObj obj, bool exists, uint64_t apos, uint64_t cpos);

    ESCTestTokens getTestESCTokens(BSONObj obj);

    ECOCCompactionDocumentV2 generateTestECOCDocumentV2(BSONObj obj);

    EncryptedFieldConfig generateEncryptedFieldConfig(
        const std::set<std::string>& encryptedFieldNames);

    BSONObj generateCompactionTokens(const std::set<std::string>& encryptedFieldNames);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

    void doSingleInsert(int id, BSONObj encryptedFieldsObj);

    void insertFieldValues(StringData fieldName, std::map<std::string, InsertionState>& values);

    void doInsertAndCompactCycles(StringData fieldName,
                                  std::map<std::string, InsertionState>& values,
                                  bool compactOnLastCycle,
                                  uint64_t cycles,
                                  uint64_t insertsPerCycle = 1);

protected:
    ServiceContext::UniqueOperationContext _opCtx;

    repl::StorageInterface* _storage{nullptr};

    std::unique_ptr<FLEQueryInterfaceMock> _queryImpl;

    std::unique_ptr<repl::UnreplicatedWritesBlock> _uwb;

    TestKeyVault _keyVault;

    EncryptedStateCollectionsNamespaces _namespaces;
};

void FleCompactTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));
    repl::ReplicationCoordinator::get(service)
        ->setFollowerMode(repl::MemberState::RS_PRIMARY)
        .ignore();

    _opCtx = cc().makeOperationContext();
    _uwb = std::make_unique<repl::UnreplicatedWritesBlock>(_opCtx.get());

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    _storage = repl::StorageInterface::get(service);

    _queryImpl = std::make_unique<FLEQueryInterfaceMock>(_opCtx.get(), _storage);

    _namespaces.edcNss = NamespaceString::createNamespaceString_forTest("test.edc");
    _namespaces.escNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.esc");
    _namespaces.ecocNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc");
    _namespaces.ecocRenameNss = NamespaceString::createNamespaceString_forTest("test.ecoc.compact");

    createCollection(_namespaces.edcNss);
    createCollection(_namespaces.escNss);
    createCollection(_namespaces.ecocNss);
}

void FleCompactTest::tearDown() {
    _uwb.reset(nullptr);
    _opCtx.reset(nullptr);
    ServiceContextMongoDTest::tearDown();
}

void FleCompactTest::createCollection(const NamespaceString& ns) {
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(),
        NamespaceString::createNamespaceString_forTest(ns.dbName(), ns.coll()),
        collectionOptions);
    ASSERT_OK(statusCC);
}

void FleCompactTest::assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecoc) {
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.edcNss), edc);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.escNss), esc);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.ecocNss), ecoc);
}

void FleCompactTest::assertESCNonAnchorDocument(BSONObj obj, bool exists, uint64_t cpos) {
    auto tokens = getTestESCTokens(obj);
    auto doc = _queryImpl->getById(
        _namespaces.escNss, ESCCollection::generateNonAnchorId(tokens.twiceDerivedTag, cpos));
    ASSERT_EQ(doc.isEmpty(), !exists);
    ASSERT(!doc.hasField("value"_sd));
}

void FleCompactTest::assertESCAnchorDocument(
    BSONObj obj, bool exists, uint64_t apos, uint64_t cpos, bool nullAnchor) {
    auto tokens = getTestESCTokens(obj);
    auto id = nullAnchor ? ESCCollection::generateNullAnchorId(tokens.twiceDerivedTag)
                         : ESCCollection::generateAnchorId(tokens.twiceDerivedTag, apos);
    auto doc = _queryImpl->getById(_namespaces.escNss, id);
    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto anchorDoc =
            uassertStatusOK(ESCCollection::decryptAnchorDocument(tokens.twiceDerivedValue, doc));
        ASSERT_EQ(anchorDoc.position, nullAnchor ? apos : 0);
        ASSERT_EQ(anchorDoc.count, cpos);
    }
}

void FleCompactTest::assertESCNullAnchorDocument(BSONObj obj,
                                                 bool exists,
                                                 uint64_t apos,
                                                 uint64_t cpos) {
    return assertESCAnchorDocument(obj, exists, apos, cpos, true);
}

FleCompactTest::ESCTestTokens FleCompactTest::getTestESCTokens(BSONObj obj) {
    auto element = obj.firstElement();
    auto indexKeyId = fieldNameToUUID(element.fieldNameStringData());
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);
    auto eltCdr = ConstDataRange(element.value(), element.value() + element.valuesize());
    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, eltCdr);

    FleCompactTest::ESCTestTokens tokens;

    tokens.contentionDerived = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);
    tokens.twiceDerivedValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(tokens.contentionDerived);
    tokens.twiceDerivedTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(tokens.contentionDerived);

    tokens.anchorPaddingRoot = FLEAnchorPaddingGenerator::generateAnchorPaddingRootToken(escToken);
    tokens.anchorPaddingKey =
        FLEAnchorPaddingDerivedGenerator::generateAnchorPaddingKeyToken(tokens.anchorPaddingRoot);
    tokens.anchorPaddingValue =
        FLEAnchorPaddingDerivedGenerator::generateAnchorPaddingValueToken(tokens.anchorPaddingRoot);

    return tokens;
}

ECOCCompactionDocumentV2 FleCompactTest::generateTestECOCDocumentV2(BSONObj obj) {
    ECOCCompactionDocumentV2 doc;
    doc.fieldName = obj.firstElementFieldName();
    doc.esc = getTestESCTokens(obj).contentionDerived;
    return doc;
}

EncryptedFieldConfig FleCompactTest::generateEncryptedFieldConfig(
    const std::set<std::string>& encryptedFieldNames) {
    BSONObjBuilder builder;
    EncryptedFieldConfig efc;
    std::vector<EncryptedField> encryptedFields;

    for (const auto& field : encryptedFieldNames) {
        EncryptedField ef;
        ef.setKeyId(fieldNameToUUID(field));
        ef.setPath(field);
        ef.setBsonType("string"_sd);
        QueryTypeConfig q(QueryTypeEnum::Equality);
        auto x = ef.getQueries();
        x = std::move(q);
        ef.setQueries(x);
        encryptedFields.push_back(std::move(ef));
    }

    efc.setEscCollection(_namespaces.escNss.coll());
    efc.setEcocCollection(_namespaces.ecocNss.coll());
    efc.setFields(std::move(encryptedFields));
    return efc;
}

BSONObj FleCompactTest::generateCompactionTokens(const std::set<std::string>& encryptedFieldNames) {
    auto efc = generateEncryptedFieldConfig(encryptedFieldNames);
    return FLEClientCrypto::generateCompactionTokens(efc, &_keyVault);
}

std::vector<char> FleCompactTest::generatePlaceholder(UUID keyId, BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(userKeyId);
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

void FleCompactTest::doSingleInsert(int id, BSONObj encryptedFieldsObj) {
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");

    for (auto&& elt : encryptedFieldsObj) {
        UUID uuid = fieldNameToUUID(elt.fieldNameStringData());
        auto buf = generatePlaceholder(uuid, elt);
        builder.appendBinData(
            elt.fieldNameStringData(), buf.size(), BinDataType::Encrypt, buf.data());
    }

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc =
        generateEncryptedFieldConfig(encryptedFieldsObj.getFieldNames<std::set<std::string>>());

    int stmtId = 0;

    uassertStatusOK(processInsert(
        _queryImpl.get(), _namespaces.edcNss, serverPayload, efc, &stmtId, result, false));
}

void FleCompactTest::insertFieldValues(StringData field,
                                       std::map<std::string, InsertionState>& values) {
    static int insertId = 1;

    for (auto& [value, state] : values) {
        for (; state.toInsertCount > 0; state.toInsertCount--) {
            BSONObjBuilder builder;
            builder.append(field, value);
            doSingleInsert(insertId, builder.obj());
            ++state.pos;
            ++state.count;
            state.insertedIds[state.count] = insertId;
            ++insertId;
        }
    }
}

void FleCompactTest::doInsertAndCompactCycles(StringData fieldName,
                                              std::map<std::string, InsertionState>& values,
                                              bool compactOnLastCycle,
                                              uint64_t cycles,
                                              uint64_t insertsPerCycle) {
    ECStats escStats;

    for (; cycles > 0; cycles--) {
        for (auto& [value, state] : values) {
            state.toInsertCount = insertsPerCycle;
        }

        insertFieldValues(fieldName, values);

        if (!compactOnLastCycle && cycles == 1) {
            break;
        }

        for (auto& [value, state] : values) {
            auto testPair = BSON(fieldName << value);
            auto ecocDoc = generateTestECOCDocumentV2(testPair);
            compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
        }
    }
}


TEST_F(FleCompactTest, GetUniqueECOCDocsFromEmptyECOC) {
    ECOCStats stats;
    std::set<std::string> fieldSet = {"first", "ssn"};
    auto tokens = generateCompactionTokens(fieldSet);
    auto docs = getUniqueCompactionDocuments(_queryImpl.get(), tokens, _namespaces.ecocNss, &stats);
    ASSERT(docs.empty());
}

TEST_F(FleCompactTest, GetUniqueECOCDocsMultipleFieldsWithManyDuplicateValues) {
    ECOCStats stats;
    std::set<std::string> fieldSet = {"first", "ssn", "city", "state", "zip"};
    int numInserted = 0;
    int uniqueValuesPerField = 10;
    stdx::unordered_set<ECOCCompactionDocumentV2> expected;

    for (auto& field : fieldSet) {
        std::map<std::string, InsertionState> values;
        for (int i = 1; i <= uniqueValuesPerField; i++) {
            auto val = "value_" + std::to_string(i);
            expected.insert(generateTestECOCDocumentV2(BSON(field << val)));
            values[val].toInsertCount = i;
            numInserted += i;
        }
        insertFieldValues(field, values);
    }

    assertDocumentCounts(numInserted, numInserted, numInserted);

    auto tokens = generateCompactionTokens(fieldSet);
    auto docs = getUniqueCompactionDocuments(_queryImpl.get(), tokens, _namespaces.ecocNss, &stats);
    ASSERT(docs == expected);
}

// Tests V2 compaction on an ESC that does not contain non-anchors
TEST_F(FleCompactTest, CompactValueV2_NoNonAnchors) {
    ECStats escStats;
    auto testPair = BSON("first"
                         << "brian");
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    assertDocumentCounts(0, 0, 0);

    // Compact an empty ESC; assert an error is thrown because compact should not be called
    // if there are no ESC entries that correspond to the ECOC document.
    // Note: this tests compact where EmuBinary returns (cpos = 0, apos = 0)
    ASSERT_THROWS_CODE(
        compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats),
        DBException,
        7666502);
    assertDocumentCounts(0, 0, 0);
}

TEST_F(FleCompactTest, CompactValueV2_NoNullAnchors) {
    ECStats escStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);

    // Insert 15 of the same value; assert non-anchors 1 thru 15
    values[value].toInsertCount = 15;
    insertFieldValues(key, values);
    assertDocumentCounts(15, 15, 15);
    for (uint64_t i = 1; i <= 15; i++) {
        assertESCNonAnchorDocument(testPair, true, i);
    }

    // Compact ESC which should only have non-anchors
    // Note: this tests compact where EmuBinary returns (cpos > 0, apos = 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(15, 16, 15);
    assertESCAnchorDocument(testPair, true, 1, 15);

    // Compact ESC which should now have a fresh anchor and stale non-anchors
    // Note: this tests compact where EmuBinary returns (cpos = null, apos > 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(15, 16, 15);

    // Insert another 15 of the same value; assert non-anchors 16 thru 30
    values[value].toInsertCount = 15;
    insertFieldValues(key, values);
    assertDocumentCounts(30, 31, 30);
    for (uint64_t i = 16; i <= 30; i++) {
        assertESCNonAnchorDocument(testPair, true, i);
    }

    // Compact ESC which should now have fresh anchors and fresh non-anchors
    // Note: this tests compact where EmuBinary returns (cpos > 0, apos > 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(30, 32, 30);
    assertESCAnchorDocument(testPair, true, 2, 30);
}

TEST_F(FleCompactTest, CompactValueV2_WithNullAnchor) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    std::map<std::string, InsertionState> values = {{value, {}}};

    uint64_t edcCount = 0;
    uint64_t escCount = 0;
    uint64_t ecocCount = 0;

    std::vector<PrfBlock> anchorsToDelete;
    size_t anchorLimit = 100;

    // Insert new documents
    values[value].toInsertCount = 5;
    insertFieldValues(key, values);
    edcCount = values[value].count;
    escCount += values[value].count;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Run cleanup on ESC to insert the null anchor
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    escCount++;
    ASSERT(anchorsToDelete.empty());
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCNullAnchorDocument(testPair, true, 0 /*apos*/, 5 /*cpos*/);

    // Compact ESC which now contains the null anchor + stale non-anchors; assert no change
    // Note: this tests compact where EmuBinary returns (cpos = null, apos = null)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Insert new documents
    values[value].toInsertCount = 5;
    insertFieldValues(key, values);
    edcCount += 5;
    escCount += 5;
    ecocCount += 5;
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Compact ESC which now has non-anchors + null anchor; assert anchor is inserted with apos=1
    // Note: this tests compact where EmuBinary returns (cpos > 0, apos = null)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    escCount++;
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCAnchorDocument(testPair, true, 1, edcCount);
}

TEST_F(FleCompactTest, RandomESCNonAnchorDeletions) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    std::map<std::string, InsertionState> values;
    size_t deleteCount = 150;
    size_t limit = deleteCount * sizeof(PrfBlock);

    // read from empty ESC; limit to 150 tags
    auto idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, limit, &escStats);
    ASSERT(idSet.empty());
    ASSERT_EQ(escStats.getRead(), 0);

    // populate the ESC with 300 non-anchors
    values[value].toInsertCount = 300;
    insertFieldValues(key, values);
    assertDocumentCounts(300, 300, 300);

    // read from non-empty ESC; limit to 0 tags
    idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, 0, &escStats);
    ASSERT(idSet.empty());
    ASSERT_EQ(escStats.getRead(), 0);

    // read from non-empty ESC; limit to 150 tags
    idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, limit, &escStats);
    ASSERT(!idSet.empty());
    ASSERT_EQ(idSet.size(), deleteCount);
    ASSERT_EQ(escStats.getRead(), deleteCount);

    // delete the tags from the ESC; 30 tags at a time
    cleanupESCNonAnchors(_opCtx.get(), _namespaces.escNss, idSet, 30, &escStats);
    ASSERT_EQ(escStats.getDeleted(), deleteCount);
    assertDocumentCounts(300, 300 - deleteCount, 300);

    // assert the deletes are scattered
    // (ie. less than 150 deleted in first half of the original set of 300)
    auto tokens = getTestESCTokens(BSON(key << value));
    int counter = 0;
    for (uint64_t i = 1; i <= deleteCount; i++) {
        auto doc = _queryImpl->getById(
            _namespaces.escNss, ESCCollection::generateNonAnchorId(tokens.twiceDerivedTag, i));
        if (!doc.isEmpty()) {
            counter++;
        }
    }
    ASSERT_LT(counter, deleteCount);
}

// Tests cleanup on an empty ESC
TEST_F(FleCompactTest, CleanupValueV2_EmptyESC) {
    ECStats escStats;
    auto testPair = BSON("first"
                         << "brian");
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    assertDocumentCounts(0, 0, 0);
    size_t anchorLimit = 100;

    // Cleanup an empty ESC; assert an error is thrown because cleanup should not be called
    // if there are no ESC entries that correspond to the ECOC document.
    // Note: this tests cleanup where EmuBinary returns (cpos = 0, apos = 0)
    ASSERT_THROWS_CODE(cleanupOneFieldValuePair(
                           _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats),
                       DBException,
                       7618816);
    assertDocumentCounts(0, 0, 0);
}

// Tests case (E) in cleanup algorithm, where apos = null and (cpos = null or cpos > 0).
// No regular anchors exist during cleanup.
TEST_F(FleCompactTest, CleanupValue_NullAnchorHasLatestApos_NoAnchors) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    std::map<std::string, InsertionState> values = {{value, {}}};

    uint64_t edcCount = 0;
    uint64_t escCount = 0;
    uint64_t ecocCount = 0;

    std::vector<PrfBlock> anchorsToDelete;
    size_t anchorLimit = 100;

    // Insert new documents
    values[value].toInsertCount = 5;
    insertFieldValues(key, values);
    edcCount = values[value].count;
    escCount += values[value].count;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Run cleanup on empty ESC to insert the null anchor with (apos = 0, cpos = 5)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    escCount++;
    ASSERT(anchorsToDelete.empty());
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCNullAnchorDocument(testPair, true, 0 /*apos*/, 5 /*cpos*/);

    // Run cleanup again; (tests emuBinary apos = null, cpos = null)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    ASSERT(anchorsToDelete.empty());
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Assert null anchor is unchanged
    assertESCNullAnchorDocument(testPair, true, 0 /*apos*/, 5 /*cpos*/);

    // Insert new documents
    values[value].toInsertCount = 5;
    insertFieldValues(key, values);
    edcCount += 5;
    escCount += 5;
    ecocCount += 5;

    // Run cleanup again; (tests emuBinary apos = null, cpos > 0)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    ASSERT(anchorsToDelete.empty());
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Assert null anchor is updated with new cpos
    assertESCNullAnchorDocument(testPair, true, 0 /*apos*/, 10 /*cpos*/);
}

// Tests case (E) in cleanup algorithm, where apos = null and (cpos = null or cpos > 0).
// Regular anchors exist during cleanup.
TEST_F(FleCompactTest, CleanupValue_NullAnchorHasLatestApos_WithAnchors) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    std::map<std::string, InsertionState> values = {{value, {}}};

    uint64_t numAnchors, edcCount, ecocCount, escCount;

    std::vector<PrfBlock> anchorsToDelete;
    size_t anchorLimit = 100;

    // Run a few insert & compact cycles to populate the ESC with non-anchors & plain anchors
    doInsertAndCompactCycles(key, values, true, 5 /*cycles*/, 25 /*insertsPerCycle*/);
    numAnchors = 5;
    edcCount = 25 * 5;
    escCount = edcCount + numAnchors;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCAnchorDocument(testPair, true, numAnchors /*apos*/, edcCount /*cpos*/);

    // Run cleanup to insert the null anchor
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    escCount++;  // null anchor inserted
    ASSERT_EQ(numAnchors, anchorsToDelete.size());
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCNullAnchorDocument(testPair, true, numAnchors, edcCount);

    // Run cleanup again; (tests emuBinary apos = null, cpos = null)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    assertDocumentCounts(edcCount, escCount, ecocCount);
    ASSERT(anchorsToDelete.empty());

    // Assert null anchor is unchanged
    assertESCNullAnchorDocument(testPair, true, numAnchors, edcCount);

    // Insert 25 new documents
    doInsertAndCompactCycles(key, values, false /*no compact*/, 1, 25);
    edcCount += 25;
    escCount += 25;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Run cleanup again; (tests emuBinary apos = null, cpos > 0)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    assertDocumentCounts(edcCount, escCount, ecocCount);
    ASSERT(anchorsToDelete.empty());

    // Assert null anchor is updated with new cpos
    assertESCNullAnchorDocument(testPair, true, numAnchors, edcCount);
}

// Tests case (F) in cleanup algorithm, where apos = 0 and (cpos > 0)
TEST_F(FleCompactTest, CleanupValue_NoAnchorsExist) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    std::map<std::string, InsertionState> values = {{value, {}}};

    uint64_t edcCount = 0;
    uint64_t escCount = 0;
    uint64_t ecocCount = 0;

    std::vector<PrfBlock> anchorsToDelete;
    size_t anchorLimit = 100;

    // Insert new documents
    values[value].toInsertCount = 5;
    insertFieldValues(key, values);
    edcCount = values[value].count;
    escCount += edcCount;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Cleanup ESC with new non-anchors; (tests emuBinary apos = 0, cpos > 0)
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    escCount++;  // null anchor inserted
    ASSERT(anchorsToDelete.empty());
    assertDocumentCounts(edcCount, escCount, ecocCount);

    // Assert null doc is inserted with apos = 0, cpos = 5
    assertESCNullAnchorDocument(testPair, true, 0, edcCount);
}

// Tests case (G) in cleanup algorithm, where apos > 0 and (cpos = null or cpos > 0)
TEST_F(FleCompactTest, CleanupValue_NewAnchorsExist) {
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    std::map<std::string, InsertionState> values = {{value, {}}};

    uint64_t numAnchors, edcCount, ecocCount, escCount;
    uint64_t escDeletesCount = 0;

    std::vector<PrfBlock> anchorsToDelete;
    size_t anchorLimit = 100;

    // Run a few insert & compact cycles to populate the ESC with non-anchors & plain anchors
    doInsertAndCompactCycles(key, values, true, 5 /*cycles*/, 25 /*insertsPerCycle*/);
    numAnchors = 5;
    edcCount = 25 * 5;
    escCount = edcCount + numAnchors;
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCAnchorDocument(testPair, true, numAnchors /*apos*/, edcCount /*cpos*/);

    // Run cleanup; (tests emuBinary apos > 0, cpos = null)
    // Assert null doc is inserted & has the latest apos & cpos
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    escCount++;  // null anchor inserted
    escDeletesCount = numAnchors;
    ASSERT_EQ(escDeletesCount, anchorsToDelete.size());
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCNullAnchorDocument(testPair, true, numAnchors, edcCount);

    // Run a few more insert & compact cycles, but don't compact on last cycle
    doInsertAndCompactCycles(key, values, false /*compactOnLastCycle*/, 5, 25);
    numAnchors += 4;
    escDeletesCount = 4;
    edcCount += (25 * 5);
    escCount += (4 + 25 * 5);
    ecocCount = edcCount;
    assertDocumentCounts(edcCount, escCount, ecocCount);
    // assert latest anchor has cpos that is 25 inserts stale.
    assertESCAnchorDocument(testPair, true, numAnchors, edcCount - 25);

    // Run cleanup; (tests emuBinary apos > 0, cpos > 0)
    // Assert null doc is updated with the latest apos & cpos
    anchorsToDelete = cleanupOneFieldValuePair(
        _queryImpl.get(), ecocDoc, _namespaces.escNss, anchorLimit, &escStats);
    ASSERT_EQ(escDeletesCount, anchorsToDelete.size());
    assertDocumentCounts(edcCount, escCount, ecocCount);
    assertESCNullAnchorDocument(testPair, true, numAnchors, edcCount);
}

template <typename T>
QueryTypeConfig generateQueryTypeConfigForTest(const T& min,
                                               const T& max,
                                               boost::optional<uint32_t> precision = boost::none,
                                               int sparsity = 1) {
    QueryTypeConfig config;
    config.setQueryType(QueryTypeEnum::Range);
    config.setMin(Value(min));
    config.setMax(Value(max));
    if (precision) {
        config.setPrecision(precision.get());
    }
    config.setSparsity(sparsity);

    return config;
}

TEST_F(FleCompactTest, InjectSomeAnchorPadding) {
    const BSONObj dataDoc = BSON("a.b.c" << 42);
    const auto tokens = getTestESCTokens(dataDoc);
    auto queryTypeConfig = generateQueryTypeConfigForTest(0, 100);

    // Expect numPads := gamma * (pathLength * uniqueLeaves - uniqueTokens)
    // numPads := 0.42 * (8 * 1 - 5) => 0.42 * 3 => 1.26 => 2 pads {1, 2}
    compactOneRangeFieldPad(_queryImpl.get(),
                            _namespaces.escNss,
                            queryTypeConfig,
                            0.42,
                            1,
                            5,
                            tokens.anchorPaddingRoot);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.escNss), 2);
}

TEST_F(FleCompactTest, InjectManyAnchorPadding) {
    const BSONObj dataDoc = BSON("a.b.c" << 42);
    const auto tokens = getTestESCTokens(dataDoc);
    auto queryTypeConfig = generateQueryTypeConfigForTest(0LL, 100LL);

    // Expect numPads := gamma * (pathLength * uniqueLeaves - uniqueTokens)
    // numPads := 1.0 * (8 * 5 - 25) => 40 - 25 => 15.0 => 15 pads {1..15}
    compactOneRangeFieldPad(
        _queryImpl.get(), _namespaces.escNss, queryTypeConfig, 1, 5, 25, tokens.anchorPaddingRoot);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.escNss), 15);
}

}  // namespace
}  // namespace mongo

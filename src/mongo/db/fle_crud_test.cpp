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


#include "mongo/db/fle_crud.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/fle_query_interface_mock.h"
#include "mongo/db/fts/unicode/string.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/murmur3.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

namespace fle {
size_t sizeArrayElementsMemory(size_t tagCount);
}

namespace {

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId));

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
    SymmetricKey& getKMSLocalKey() override {
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

std::string fieldNameFromInt(uint64_t i) {
    return "field" + std::to_string(i);
}

int32_t getTestSeed() {
    static std::unique_ptr<mongo::PseudoRandom> rnd;
    if (!rnd.get()) {
        auto seed = SecureRandom().nextInt64();
        rnd = std::make_unique<mongo::PseudoRandom>(seed);
        std::cout << "FLE TEST SEED: " << seed << std::endl;
    }

    return rnd->nextInt32();
}
class FleCrudTest : public ServiceContextMongoDTest {
protected:
    void setUp() override;
    void tearDown() override;

    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecoc);

    void testValidateEncryptedFieldInfo(BSONObj obj, bool bypassValidation);

    void testValidateTags(BSONObj obj);

    void doSingleInsert(int id,
                        BSONElement element,
                        Fle2AlgorithmInt alg,
                        bool bypassDocumentValidation = false);
    void doSingleInsert(int id, BSONObj obj, bool bypassDocumentValidation = false);

    void doSingleInsertWithContention(
        int id, BSONElement element, int64_t cm, uint64_t cf, EncryptedFieldConfig efc);
    void doSingleInsertWithContention(
        int id, BSONObj obj, int64_t cm, uint64_t cf, EncryptedFieldConfig efc);

    void doSingleDelete(int id, Fle2AlgorithmInt alg);

    void doSingleUpdate(int id, BSONElement element, Fle2AlgorithmInt alg);
    void doSingleUpdate(int id, BSONObj obj);
    void doSingleUpdateWithUpdateDoc(int id, BSONObj update, Fle2AlgorithmInt alg);
    void doSingleUpdateWithUpdateDoc(int id,
                                     const write_ops::UpdateModification& modification,
                                     Fle2AlgorithmInt);

    void doFindAndModify(write_ops::FindAndModifyCommandRequest& request, Fle2AlgorithmInt alg);

    using ValueGenerator = std::function<std::string(StringData fieldName, uint64_t row)>;

    void doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func);

    void validateDocument(int id, boost::optional<BSONObj> doc, Fle2AlgorithmInt alg);

    ESCDerivedFromDataToken getTestESCDataToken(BSONObj obj);
    EDCDerivedFromDataToken getTestEDCDataToken(BSONObj obj);

    ESCTwiceDerivedTagToken getTestESCToken(BSONElement value);
    ESCTwiceDerivedTagToken getTestESCToken(BSONObj obj);
    ESCTwiceDerivedTagToken getTestESCToken(StringData name, StringData value);

    void assertECOCDocumentCountByField(StringData fieldName, uint64_t expect);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

    BSONObj transformElementForInsertUpdate(BSONElement element,
                                            const std::vector<char>& placeholder,
                                            const EncryptedFieldConfig& efc);

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

    NamespaceString _edcNs =
        NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.edc");
    NamespaceString _escNs =
        NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.esc");
    NamespaceString _ecocNs =
        NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc");

    HmacContext hmacCtx;
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
    createCollection(_ecocNs);
}

void FleCrudTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

void FleCrudTest::createCollection(const NamespaceString& ns) {
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();

    // Make the state collections clustered sometimes, allows us to ensure the tags reading code can
    // handle clustered and non-clustered state collections
    if (ns != _edcNs) {
        auto seed = getTestSeed();
        if (seed % 2 == 0) {
            collectionOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        }
    }

    auto statusCC = _storage->createCollection(
        _opCtx.get(),
        NamespaceString::createNamespaceString_forTest(ns.dbName(), ns.coll()),
        collectionOptions);
    ASSERT_OK(statusCC);
}

ConstDataRange toCDR(BSONElement element) {
    return ConstDataRange(element.value(), element.value() + element.valuesize());
}

ESCDerivedFromDataToken FleCrudTest::getTestESCDataToken(BSONObj obj) {
    auto element = obj.firstElement();
    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = ESCToken::deriveFrom(c1token);
    return ESCDerivedFromDataToken::deriveFrom(escToken, toCDR(element));
}

EDCDerivedFromDataToken FleCrudTest::getTestEDCDataToken(BSONObj obj) {
    auto element = obj.firstElement();
    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(indexKeyId).key);
    auto edcToken = EDCToken::deriveFrom(c1token);
    return EDCDerivedFromDataToken::deriveFrom(edcToken, toCDR(element));
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONElement element) {
    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = ESCToken::deriveFrom(c1token);
    auto escDataToken = ESCDerivedFromDataToken::deriveFrom(escToken, toCDR(element));
    auto escContentionToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDataToken, 0);

    return ESCTwiceDerivedTagToken::deriveFrom(escContentionToken);
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONObj obj) {
    return getTestESCToken(obj.firstElement());
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(StringData name, StringData value) {

    auto doc = BSON("i" << value);
    auto element = doc.firstElement();

    UUID keyId = fieldNameToUUID(name);

    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(keyId).key);
    auto escToken = ESCToken::deriveFrom(c1token);

    auto escDataToken = ESCDerivedFromDataToken::deriveFrom(escToken, toCDR(element));
    auto escContentionToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDataToken, 0);

    return ESCTwiceDerivedTagToken::deriveFrom(escContentionToken);
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

    constexpr auto schemaV2 = R"({
        "escCollection": "enxcol_.coll.esc",
        "ecocCollection": "enxcol_.coll.ecoc",
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

    constexpr auto rangeSchemaV2 = R"({
        "escCollection": "enxcol_.coll.esc",
        "ecocCollection": "enxcol_.coll.ecoc",
        "fields": [
            {
                "keyId":
                                {
                                    "$uuid": "12345678-1234-9876-1234-123456789012"
                                }
                            ,
                "path": "encrypted",
                "bsonType": "int",
                "queries": {"queryType": "range", "min": 0, "max": 15, "sparsity": 1, "trimFactor": 0}

            }
        ]
    })";

    if (alg == Fle2AlgorithmInt::kEquality) {
        return EncryptedFieldConfig::parse(fromjson(schemaV2), IDLParserContext("root"));
    }
    return EncryptedFieldConfig::parse(fromjson(rangeSchemaV2), IDLParserContext("root"));
}

void parseEncryptedInvalidFieldConfig(StringData esc, StringData ecoc) {

    auto invalidCollectionNameSchema =
        // "{" +
        fmt::format("{{\"escCollection\": \"{}\", \"ecocCollection\": \"{}\", ", esc, ecoc) +
        R"(
        "fields": [
            {
                "keyId":
                                {
                                    "$uuid": "12345678-1234-9876-1234-123456789012"
                                }
                            ,
                "path": "encrypted",
                "bsonType": "int",
                "queries": {"queryType": "range", "min": 0, "max": 15, "sparsity": 1, "trimFactor": 0}

            }
        ]
    })";

    EncryptedFieldConfig::parse(fromjson(invalidCollectionNameSchema), IDLParserContext("root"));
}

void FleCrudTest::assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecoc) {
    ASSERT_EQ(_queryImpl->countDocuments(_edcNs), edc);
    ASSERT_EQ(_queryImpl->countDocuments(_escNs), esc);
    ASSERT_EQ(_queryImpl->countDocuments(_ecocNs), ecoc);
}

// Auto generate key ids from field id
void FleCrudTest::doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func) {
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");

    auto efc = getTestEncryptedFieldConfig();

    for (uint64_t i = 0; i < fieldCount; i++) {
        auto name = fieldNameFromInt(i);
        auto value = func(name, id);
        auto doc = BSON(name << value);
        UUID uuid = fieldNameToUUID(name);
        auto buf = generatePlaceholder(uuid, doc.firstElement());
        builder.append(
            transformElementForInsertUpdate(doc.firstElement(), buf, efc).firstElement());
    }
    auto result = builder.obj();

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}


void FleCrudTest::validateDocument(int id, boost::optional<BSONObj> doc, Fle2AlgorithmInt alg) {

    auto doc1 = BSON("_id" << id);
    auto updatedDoc = _queryImpl->getById(_edcNs, doc1.firstElement());

    std::cout << "Updated Doc: " << updatedDoc << std::endl;

    auto efc = getTestEncryptedFieldConfig(alg);

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
    spec.setMinBound(boost::optional<IDLAnyType>(lowerDoc.firstElement()));
    auto upperDoc = BSON("ub" << 15);

    spec.setMaxBound(boost::optional<IDLAnyType>(upperDoc.firstElement()));
    spec.setTrimFactor(0);
    auto specDoc = BSON("s" << spec.toBSON());

    return specDoc;
}

// Use different keys for index and user
std::vector<char> generateSinglePlaceholder(BSONElement value,
                                            Fle2AlgorithmInt alg = Fle2AlgorithmInt::kEquality,
                                            int64_t cm = 0) {

    FLE2EncryptionPlaceholder ep;
    ep.setAlgorithm(alg);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);

    // Keep definition outside of conditional to keep it alive until serialization.
    BSONObj temp;
    if (alg == Fle2AlgorithmInt::kRange) {
        temp = generateFLE2RangeInsertSpec(value);
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

// Given a BSON element containing the plaintext value and fieldname, the placeholder
// generated for the value, and the EncryptedFieldConfig with a schema for the fieldname,
// returns a document {fieldname: BinData(<InsertUpdatePayloadV2>))}
BSONObj FleCrudTest::transformElementForInsertUpdate(BSONElement element,
                                                     const std::vector<char>& placeholder,
                                                     const EncryptedFieldConfig& efc) {
    // Wrap the element in a document in an insert command, so libmongocrypt can transform
    // the placeholders.
    auto origCmd = write_ops::InsertCommandRequest(_edcNs, {element.wrap()}).toBSON();
    auto cryptdResponse = [&]() {
        BSONObjBuilder docbob;
        docbob.appendBinData(element.fieldNameStringData(),
                             placeholder.size(),
                             BinDataType::Encrypt,
                             placeholder.data());
        BSONObjBuilder bob;
        bob.append("hasEncryptionPlaceholders", true);
        bob.append("schemaRequiresEncryption", true);
        bob.append("result", write_ops::InsertCommandRequest(_edcNs, {docbob.obj()}).toBSON());
        return bob.obj();
    }();
    auto finalCmd =
        FLEClientCrypto::transformPlaceholders(origCmd,
                                               cryptdResponse,
                                               BSON(_edcNs.toString_forTest() << efc.toBSON()),
                                               &_keyVault,
                                               _edcNs.db_forTest())
            .addField(BSON("$db" << _edcNs.db_forTest()).firstElement());
    return write_ops::InsertCommandRequest::parse(finalCmd, IDLParserContext("finalCmd"))
        .getDocuments()
        .front()
        .getOwned();
}

void FleCrudTest::doSingleInsert(int id,
                                 BSONElement element,
                                 Fle2AlgorithmInt alg,
                                 bool bypassDocumentValidation) {

    auto buf = generateSinglePlaceholder(element, alg);
    auto efc = getTestEncryptedFieldConfig(alg);

    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.append(transformElementForInsertUpdate(element, buf, efc).firstElement());
    auto result = builder.obj();

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}

void FleCrudTest::doSingleInsert(int id, BSONObj obj, bool bypassDocumentValidation) {
    doSingleInsert(id, obj.firstElement(), Fle2AlgorithmInt::kEquality);
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
    doSingleUpdate(id, obj.firstElement(), Fle2AlgorithmInt::kEquality);
}

void FleCrudTest::doSingleUpdate(int id, BSONElement element, Fle2AlgorithmInt alg) {
    auto buf = generateSinglePlaceholder(element, alg);
    auto efc = getTestEncryptedFieldConfig(alg);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", transformElementForInsertUpdate(element, buf, efc));

    auto result = builder.obj();

    doSingleUpdateWithUpdateDoc(id, result, alg);
}

void FleCrudTest::doSingleUpdateWithUpdateDoc(int id, BSONObj update, Fle2AlgorithmInt alg) {
    doSingleUpdateWithUpdateDoc(
        id,
        write_ops::UpdateModification(update, write_ops::UpdateModification::ModifierUpdateTag{}),
        alg);
}

void FleCrudTest::doSingleUpdateWithUpdateDoc(int id,
                                              const write_ops::UpdateModification& modification,
                                              Fle2AlgorithmInt alg) {

    auto efc = getTestEncryptedFieldConfig(alg);

    auto doc = EncryptionInformationHelpers::encryptionInformationSerialize(_edcNs, efc);

    auto ei = EncryptionInformation::parse(doc, IDLParserContext("test"));

    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setU(modification);

    write_ops::UpdateCommandRequest updateRequest(_edcNs);
    updateRequest.setUpdates({entry});
    updateRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx.get())
                      .ns(updateRequest.getNamespace())
                      .runtimeConstants(updateRequest.getLegacyRuntimeConstants())
                      .letParameters(updateRequest.getLet())
                      .build();
    processUpdate(_queryImpl.get(), expCtx, updateRequest);
}

void FleCrudTest::doSingleDelete(int id, Fle2AlgorithmInt alg) {

    auto efc = getTestEncryptedFieldConfig(alg);

    auto doc = EncryptionInformationHelpers::encryptionInformationSerialize(_edcNs, efc);

    auto ei = EncryptionInformation::parse(doc, IDLParserContext("test"));

    write_ops::DeleteOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setMulti(false);

    write_ops::DeleteCommandRequest deleteRequest(_edcNs);
    deleteRequest.setDeletes({entry});
    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx.get())
                      .ns(deleteRequest.getNamespace())
                      .runtimeConstants(deleteRequest.getLegacyRuntimeConstants())
                      .letParameters(deleteRequest.getLet())
                      .build();
    processDelete(_queryImpl.get(), expCtx, deleteRequest);
}

void FleCrudTest::doFindAndModify(write_ops::FindAndModifyCommandRequest& request,
                                  Fle2AlgorithmInt alg) {
    auto efc = getTestEncryptedFieldConfig(alg);

    auto doc = EncryptionInformationHelpers::encryptionInformationSerialize(_edcNs, efc);

    auto ei = EncryptionInformation::parse(doc, IDLParserContext("test"));

    request.setEncryptionInformation(ei);
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx.get())
                      .ns(request.getNamespace())
                      .runtimeConstants(request.getLegacyRuntimeConstants())
                      .letParameters(request.getLet())
                      .build();
    processFindAndModify(expCtx, _queryImpl.get(), request);
}

class CollectionReader : public FLEStateCollectionReader {
public:
    CollectionReader(std::string&& coll, FLEQueryInterfaceMock& queryImpl)
        : _coll(NamespaceString::createNamespaceString_forTest(coll)), _queryImpl(queryImpl) {}

    uint64_t getDocumentCount() const override {
        return _queryImpl.countDocuments(_coll);
    }

    BSONObj getById(PrfBlock block) const override {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        return _queryImpl.getById(_coll, doc.firstElement());
    }

    ECStats getStats() const override {
        return ECStats();
    }

private:
    NamespaceString _coll;
    FLEQueryInterfaceMock& _queryImpl;
};

class FleTagsTest : public FleCrudTest {
protected:
    void setUp() override {
        FleCrudTest::setUp();
    }
    void tearDown() override {
        FleCrudTest::tearDown();
    }

    std::vector<std::vector<FLEEdgeCountInfo>> getCountInfoSets(BSONObj obj, uint64_t cm = 0) {
        return mongo::fle::getCountInfoSets(
            _queryImpl.get(), _escNs, getTestESCDataToken(obj), getTestEDCDataToken(obj), cm);
    }

    std::vector<PrfBlock> readTags(BSONObj obj, uint64_t cm = 0) {
        return mongo::fle::readTags(
            _queryImpl.get(), _escNs, getTestESCDataToken(obj), getTestEDCDataToken(obj), cm);
    }
};

// Insert one document
TEST_F(FleCrudTest, InsertOne) {
    auto doc = BSON("encrypted" << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(1, 1, 1);
    assertECOCDocumentCountByField("encrypted", 1);

    ASSERT_FALSE(
        _queryImpl
            ->getById(_escNs,
                      ESCCollection::generateNonAnchorId(&hmacCtx, getTestESCToken(element), 1))
            .isEmpty());
}

TEST_F(FleCrudTest, InsertOneRange) {
    auto doc = BSON("encrypted" << 5);
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kRange);
    assertDocumentCounts(1, 5, 5);
    assertECOCDocumentCountByField("encrypted", 5);
}

// Insert two documents with same values
TEST_F(FleCrudTest, InsertTwoSame) {

    auto doc = BSON("encrypted" << "secret");
    auto element = doc.firstElement();
    doSingleInsert(1, element, Fle2AlgorithmInt::kEquality);
    doSingleInsert(2, element, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(2, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    auto escTagToken = getTestESCToken(element);
    ASSERT_FALSE(
        _queryImpl->getById(_escNs, ESCCollection::generateNonAnchorId(&hmacCtx, escTagToken, 1))
            .isEmpty());
    ASSERT_FALSE(
        _queryImpl->getById(_escNs, ESCCollection::generateNonAnchorId(&hmacCtx, escTagToken, 2))
            .isEmpty());
}

// Insert two documents with different values
TEST_F(FleCrudTest, InsertTwoDifferent) {

    doSingleInsert(1, BSON("encrypted" << "secret"));
    doSingleInsert(2, BSON("encrypted" << "topsecret"));

    assertDocumentCounts(2, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateNonAnchorId(
                                   &hmacCtx, getTestESCToken(BSON("encrypted" << "secret")), 1))
                     .isEmpty());
    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateNonAnchorId(
                                   &hmacCtx, getTestESCToken(BSON("encrypted" << "topsecret")), 1))
                     .isEmpty());
}

// Insert 1 document with 100 fields
TEST_F(FleCrudTest, Insert100Fields) {

    uint64_t fieldCount = 100;
    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return std::string{fieldName};
    };
    doSingleWideInsert(1, fieldCount, valueGenerator);

    assertDocumentCounts(1, fieldCount, fieldCount);

    for (uint64_t field = 0; field < fieldCount; field++) {
        auto fieldName = fieldNameFromInt(field);

        assertECOCDocumentCountByField(fieldName, 1);

        ASSERT_FALSE(_queryImpl
                         ->getById(_escNs,
                                   ESCCollection::generateNonAnchorId(
                                       &hmacCtx,
                                       getTestESCToken(fieldName,
                                                       valueGenerator(fieldNameFromInt(field), 0)),
                                       1))
                         .isEmpty());
    }
}

// Insert 100 documents each with 20 fields with 7 distinct values per field
TEST_F(FleCrudTest, Insert20Fields50Rows) {

    uint64_t fieldCount = 20;
    uint64_t rowCount = 50;

    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return std::string{fieldName} + std::to_string(row % 7);
    };


    for (uint64_t row = 0; row < rowCount; row++) {
        doSingleWideInsert(row, fieldCount, valueGenerator);
    }

    assertDocumentCounts(rowCount, rowCount * fieldCount, rowCount * fieldCount);

    for (uint64_t row = 0; row < rowCount; row++) {
        for (uint64_t field = 0; field < fieldCount; field++) {
            auto fieldName = fieldNameFromInt(field);

            int count = (row / 7) + 1;

            assertECOCDocumentCountByField(fieldName, rowCount);
            ASSERT_FALSE(
                _queryImpl
                    ->getById(_escNs,
                              ESCCollection::generateNonAnchorId(
                                  &hmacCtx,
                                  getTestESCToken(fieldName,
                                                  valueGenerator(fieldNameFromInt(field), row)),
                                  count))
                    .isEmpty());
        }
    }
}

// Test v1 FLE2InsertUpdatePayload is rejected if v2 is enabled.
// There are 2 places where the payload version compatibility is checked:
// 1. When parsing the InsertUpdatePayload in EDCServerCollection::getEncryptedFieldInfo()
// 2. When transforming the InsertUpdatePayload to the on-disk format in processInsert()
TEST_F(FleCrudTest, InsertV1PayloadAgainstV2Protocol) {

    std::vector<uint8_t> buf(64);
    buf[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2InsertUpdatePayload);

    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    BSONObj document = builder.obj();
    ASSERT_THROWS_CODE(EDCServerCollection::getEncryptedFieldInfo(document), DBException, 7291901);

    auto bogusEncryptedTokens = StateCollectionTokensV2({{}}, false, boost::none).encrypt({{}});

    FLE2InsertUpdatePayloadV2 payload;
    payload.setEdcDerivedToken({{}});
    payload.setEscDerivedToken({{}});
    payload.setServerDerivedFromDataToken({{}});
    payload.setServerEncryptionToken({{}});
    payload.setEncryptedTokens(bogusEncryptedTokens);
    payload.setValue(buf);
    payload.setType(stdx::to_underlying(BSONType::string));

    std::vector<EDCServerPayloadInfo> serverPayload(1);
    serverPayload.front().fieldPathName = "encrypted";
    serverPayload.front().counts = {1};
    serverPayload.front().payload = std::move(payload);

    auto efc = getTestEncryptedFieldConfig();
    ASSERT_THROWS_CODE(
        processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, document, false),
        DBException,
        6379103);
}

// Test insert of v1 FLEUnindexedEncryptedValue is rejected if v2 is enabled.
// There are 2 places where the payload version compatibility is checked:
// 1. When visiting all encrypted BinData in EDCServerCollection::getEncryptedFieldInfo()
// 2. When visiting all encrypted BinData in processInsert()
TEST_F(FleCrudTest, InsertUnindexedV1AgainstV2Protocol) {

    // Create a dummy InsertUpdatePayloadV2 to include in the document.
    // This is so that the assertion being tested will not be skipped during processInsert()
    auto bogusEncryptedTokens = StateCollectionTokensV2({{}}, false, boost::none).encrypt({{}});
    FLE2InsertUpdatePayloadV2 payload;
    payload.setEdcDerivedToken({{}});
    payload.setEscDerivedToken({{}});
    payload.setServerDerivedFromDataToken({{}});
    payload.setServerEncryptionToken({{}});
    payload.setEncryptedTokens(bogusEncryptedTokens);
    payload.setValue(std::vector<uint8_t>{64});
    payload.setType(stdx::to_underlying(BSONType::string));
    payload.setContentionFactor(0);
    payload.setIndexKeyId(indexKeyId);
    auto iup = payload.toBSON();
    std::vector<uint8_t> buf(iup.objsize() + 1);
    buf[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2InsertUpdatePayloadV2);
    std::copy(iup.objdata(), iup.objdata() + iup.objsize(), buf.data() + 1);

    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    // Append the unindexed v1 blob
    buf[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValue);
    builder.appendBinData("unindexed", buf.size(), BinDataType::Encrypt, buf.data());

    BSONObj document = builder.obj();

    // I. Verify the document gets rejected in getEncryptedFieldInfo()
    ASSERT_THROWS_CODE(EDCServerCollection::getEncryptedFieldInfo(document), DBException, 7413901);

    // II. Verify the document gets rejected in processInsert()
    std::vector<EDCServerPayloadInfo> serverPayload(1);
    serverPayload.front().fieldPathName = "encrypted";
    serverPayload.front().counts = {1};
    serverPayload.front().payload = std::move(payload);

    auto efc = getTestEncryptedFieldConfig();
    ASSERT_THROWS_CODE(
        processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, document, false),
        DBException,
        6379103);
}

// Test insert update payloads containing both the range edgeTokenSet array ('g') and the text
// search token sets ('b') is rejected.
TEST_F(FleCrudTest, InsertPayloadIsBothRangeAndTextSearch) {
    auto bogusEncryptedTokens = StateCollectionTokensV2({{}}, false, boost::none).encrypt({{}});
    FLE2InsertUpdatePayloadV2 payload({},
                                      {},
                                      bogusEncryptedTokens,
                                      indexKeyId,
                                      stdx::to_underlying(BSONType::string),
                                      {},
                                      {},
                                      {},
                                      0);
    payload.setEdgeTokenSet(std::vector<EdgeTokenSetV2>{{{}, {}, {}, bogusEncryptedTokens}});
    payload.setTextSearchTokenSets(
        TextSearchTokenSets{{{}, {}, {}, bogusEncryptedTokens}, {}, {}, {}});
    auto iup = payload.toBSON();
    std::vector<uint8_t> buf(iup.objsize() + 1);
    buf[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2InsertUpdatePayloadV2);
    std::copy(iup.objdata(), iup.objdata() + iup.objsize(), buf.data() + 1);
    BSONObjBuilder builder;
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());
    BSONObj document = builder.obj();

    ASSERT_THROWS_CODE(EDCServerCollection::getEncryptedFieldInfo(document), DBException, 9783801);
}

// Insert and delete one document
TEST_F(FleCrudTest, InsertAndDeleteOne) {
    auto doc = BSON("encrypted" << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(1, 1, 1);

    ASSERT_FALSE(
        _queryImpl
            ->getById(_escNs,
                      ESCCollection::generateNonAnchorId(&hmacCtx, getTestESCToken(element), 1))
            .isEmpty());

    doSingleDelete(1, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(0, 1, 1);
    assertECOCDocumentCountByField("encrypted", 1);
}

// Insert and delete one document
TEST_F(FleCrudTest, InsertAndDeleteOneRange) {
    auto doc = BSON("encrypted" << 5);
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 5, 5);

    doSingleDelete(1, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(0, 5, 5);
    assertECOCDocumentCountByField("encrypted", 5);
}

// Insert two documents, and delete both
TEST_F(FleCrudTest, InsertTwoSameAndDeleteTwo) {
    auto doc = BSON("encrypted" << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kEquality);
    doSingleInsert(2, element, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(2, 2, 2);

    ASSERT_FALSE(
        _queryImpl
            ->getById(_escNs,
                      ESCCollection::generateNonAnchorId(&hmacCtx, getTestESCToken(element), 1))
            .isEmpty());

    doSingleDelete(2, Fle2AlgorithmInt::kEquality);
    doSingleDelete(1, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(0, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);
}

// Insert two documents with different values and delete them
TEST_F(FleCrudTest, InsertTwoDifferentAndDeleteTwo) {
    doSingleInsert(1, BSON("encrypted" << "secret"));
    doSingleInsert(2, BSON("encrypted" << "topsecret"));

    assertDocumentCounts(2, 2, 2);

    doSingleDelete(2, Fle2AlgorithmInt::kEquality);
    doSingleDelete(1, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(0, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);
}

// Insert one document but delete another document
TEST_F(FleCrudTest, InsertOneButDeleteAnother) {
    doSingleInsert(1, BSON("encrypted" << "secret"));
    assertDocumentCounts(1, 1, 1);

    doSingleDelete(2, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(1, 1, 1);
    assertECOCDocumentCountByField("encrypted", 1);
}

// Update one document
TEST_F(FleCrudTest, UpdateOne) {

    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    doSingleUpdate(1, BSON("encrypted" << "top secret"));

    assertDocumentCounts(1, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "top secret"),
                     Fle2AlgorithmInt::kEquality);
}

TEST_F(FleCrudTest, UpdateOneRange) {
    auto doc = BSON("encrypted" << 5);
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 5, 5);

    auto doc2 = BSON("encrypted" << 2);
    auto elem2 = doc2.firstElement();

    doSingleUpdate(1, elem2, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 10, 10);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted" << 2),
                     Fle2AlgorithmInt::kRange);
}

// Update one document but to the same value
TEST_F(FleCrudTest, UpdateOneSameValue) {
    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    doSingleUpdate(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "secret"),
                     Fle2AlgorithmInt::kEquality);
}

// Update one document with replacement
TEST_F(FleCrudTest, UpdateOneReplace) {
    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    auto replace = BSON("encrypted" << "top secret");

    auto buf = generateSinglePlaceholder(replace.firstElement());
    auto efc = getTestEncryptedFieldConfig();

    auto result =
        BSON("plainText"
             << "fake"
             << transformElementForInsertUpdate(replace.firstElement(), buf, efc).firstElement());

    doSingleUpdateWithUpdateDoc(
        1,
        write_ops::UpdateModification(result, write_ops::UpdateModification::ReplacementTag{}),
        Fle2AlgorithmInt::kEquality);


    assertDocumentCounts(1, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    validateDocument(1,
                     BSON("_id" << 1 << "plainText"
                                << "fake"
                                << "encrypted"
                                << "top secret"),
                     Fle2AlgorithmInt::kEquality);
}

TEST_F(FleCrudTest, UpdateOneReplaceRange) {
    auto doc = BSON("encrypted" << 5);
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 5, 5);

    auto replace = BSON("encrypted" << 2);
    auto buf = generateSinglePlaceholder(replace.firstElement(), Fle2AlgorithmInt::kRange);
    auto efc = getTestEncryptedFieldConfig(Fle2AlgorithmInt::kRange);

    auto result =
        BSON("plaintext"
             << "fake"
             << transformElementForInsertUpdate(replace.firstElement(), buf, efc).firstElement());

    doSingleUpdateWithUpdateDoc(
        1,
        write_ops::UpdateModification(result, write_ops::UpdateModification::ReplacementTag{}),
        Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 10, 10);

    validateDocument(1,
                     BSON("_id" << 1 << "plaintext"
                                << "fake"
                                << "encrypted" << 2),
                     Fle2AlgorithmInt::kRange);
}

// Rename safeContent
TEST_F(FleCrudTest, RenameSafeContent) {

    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$rename", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(
        doSingleUpdateWithUpdateDoc(1, result, Fle2AlgorithmInt::kEquality), DBException, 6371506);
}

// Mess with __safeContent__ and ensure the update errors
TEST_F(FleCrudTest, SetSafeContent) {
    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(
        doSingleUpdateWithUpdateDoc(1, result, Fle2AlgorithmInt::kEquality), DBException, 6666200);
}

// Test that EDCServerCollection::validateEncryptedFieldInfo checks that the
// safeContent cannot be present in the BSON obj.
TEST_F(FleCrudTest, testValidateEncryptedFieldConfig) {
    testValidateEncryptedFieldInfo(BSON(kSafeContent << "secret"), true);
    ASSERT_THROWS_CODE(testValidateEncryptedFieldInfo(BSON(kSafeContent << "secret"), false),
                       DBException,
                       6666200);
}

// Test that EDCServerCollection::validateEncryptedFieldInfo throws an error when collection names
// do not match naming rules.
TEST_F(FleCrudTest, testValidateEncryptedFieldConfigFields) {
    ASSERT_THROWS_CODE(parseEncryptedInvalidFieldConfig("enxcol_.coll.esc1", "enxcol_.coll.ecoc"),
                       DBException,
                       7406900);
    ASSERT_THROWS_CODE(parseEncryptedInvalidFieldConfig("enxcol_.coll.esc", "enxcol_.coll.ecoc1"),
                       DBException,
                       7406902);
}

// Update one document via findAndModify
TEST_F(FleCrudTest, FindAndModify_UpdateOne) {
    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    auto doc = BSON("encrypted" << "top secret");
    auto element = doc.firstElement();
    auto buf = generateSinglePlaceholder(element);
    auto efc = getTestEncryptedFieldConfig();

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", transformElementForInsertUpdate(element, buf, efc));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));
    doFindAndModify(req, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(1, 2, 2);
    assertECOCDocumentCountByField("encrypted", 2);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "top secret"),
                     Fle2AlgorithmInt::kEquality);
}

// Update one document via findAndModify
TEST_F(FleCrudTest, FindAndModify_UpdateOneRange) {

    auto firstDoc = BSON("encrypted" << 5);

    doSingleInsert(1, firstDoc.firstElement(), Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 5, 5);

    auto doc = BSON("encrypted" << 2);
    auto element = doc.firstElement();
    auto buf = generateSinglePlaceholder(element, Fle2AlgorithmInt::kRange);
    auto efc = getTestEncryptedFieldConfig(Fle2AlgorithmInt::kRange);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", transformElementForInsertUpdate(element, buf, efc));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));
    doFindAndModify(req, Fle2AlgorithmInt::kRange);

    assertDocumentCounts(1, 10, 10);
    assertECOCDocumentCountByField("encrypted", 10);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted" << 2),
                     Fle2AlgorithmInt::kRange);
}

// Insert and delete one document via findAndModify
TEST_F(FleCrudTest, FindAndModify_InsertAndDeleteOne) {
    auto doc = BSON("encrypted" << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(1, 1, 1);

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setRemove(true);
    doFindAndModify(req, Fle2AlgorithmInt::kEquality);

    assertDocumentCounts(0, 1, 1);
    assertECOCDocumentCountByField("encrypted", 1);
}

// Rename safeContent
TEST_F(FleCrudTest, FindAndModify_RenameSafeContent) {

    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$rename", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));

    ASSERT_THROWS_CODE(doFindAndModify(req, Fle2AlgorithmInt::kEquality), DBException, 6371506);
}

TEST_F(FleCrudTest, validateTagsTest) {
    testValidateTags(BSON(kSafeContent << BSON_ARRAY(123)));
    ASSERT_THROWS_CODE(testValidateTags(BSON(kSafeContent << "foo")), DBException, 6371507);
}

// Mess with __safeContent__ and ensure the update errors
TEST_F(FleCrudTest, FindAndModify_SetSafeContent) {
    doSingleInsert(1, BSON("encrypted" << "secret"));

    assertDocumentCounts(1, 1, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    write_ops::FindAndModifyCommandRequest req(_edcNs);
    req.setQuery(BSON("_id" << 1));
    req.setUpdate(
        write_ops::UpdateModification(result, write_ops::UpdateModification::ModifierUpdateTag{}));

    ASSERT_THROWS_CODE(doFindAndModify(req, Fle2AlgorithmInt::kEquality), DBException, 6666200);
}

BSONObj makeInsertUpdatePayload(StringData path, const UUID& uuid) {
    // Actual values don't matter for these tests (apart from indexKeyId).
    auto encryptedTokens = StateCollectionTokensV2({{}}, boost::none, boost::none).encrypt({{}});
    auto bson = FLE2InsertUpdatePayloadV2({},
                                          {},
                                          std::move(encryptedTokens),
                                          uuid,
                                          stdx::to_underlying(BSONType::string),
                                          {},
                                          {},
                                          {},
                                          0)
                    .toBSON();
    std::vector<std::uint8_t> bindata;
    bindata.resize(bson.objsize() + 1);
    bindata[0] = static_cast<std::uint8_t>(EncryptedBinDataType::kFLE2InsertUpdatePayloadV2);
    memcpy(bindata.data() + 1, bson.objdata(), bson.objsize());

    BSONObjBuilder bob;
    bob.appendBinData(path, bindata.size(), BinDataType::Encrypt, bindata.data());
    return bob.obj();
}

TEST_F(FleCrudTest, validateIndexKeyValid) {
    // This test assumes we have at least one field in EFC.
    auto fields = getTestEncryptedFieldConfig().getFields();
    ASSERT_GTE(fields.size(), 1);
    auto field = fields[0];

    auto validInsert = makeInsertUpdatePayload(field.getPath(), field.getKeyId());
    auto validPayload = EDCServerCollection::getEncryptedFieldInfo(validInsert);
    validateInsertUpdatePayloads(_opCtx.get(), fields, validPayload);
}

TEST_F(FleCrudTest, validateIndexKeyInvalid) {
    // This test assumes we have at least one field in EFC.
    auto fields = getTestEncryptedFieldConfig().getFields();
    ASSERT_GTE(fields.size(), 1);
    auto field = fields[0];

    auto invalidInsert = makeInsertUpdatePayload(field.getPath(), UUID::gen());
    auto invalidPayload = EDCServerCollection::getEncryptedFieldInfo(invalidInsert);
    ASSERT_THROWS_WITH_CHECK(validateInsertUpdatePayloads(_opCtx.get(), fields, invalidPayload),
                             DBException,
                             [&](const DBException& ex) {
                                 ASSERT_STRING_CONTAINS(ex.what(),
                                                        str::stream()
                                                            << "Mismatched keyId for field '"
                                                            << field.getPath() << "'");
                             });
}

TEST_F(FleTagsTest, InsertOne) {
    auto doc = BSON("encrypted" << "a");

    doSingleInsert(1, doc);

    ASSERT_EQ(1, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoSame) {
    auto doc = BSON("encrypted" << "a");

    doSingleInsert(1, doc);
    doSingleInsert(2, doc);

    ASSERT_EQ(2, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoDifferent) {
    auto doc1 = BSON("encrypted" << "a");
    auto doc2 = BSON("encrypted" << "b");

    doSingleInsert(1, doc1);
    doSingleInsert(2, doc2);

    ASSERT_EQ(1, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, InsertAndDeleteOne) {
    auto doc = BSON("encrypted" << "a");

    doSingleInsert(1, doc);
    doSingleDelete(1, Fle2AlgorithmInt::kEquality);

    ASSERT_EQ(1, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoSameAndDeleteOne) {
    auto doc = BSON("encrypted" << "a");

    doSingleInsert(1, doc);
    doSingleInsert(2, doc);
    doSingleDelete(2, Fle2AlgorithmInt::kEquality);

    ASSERT_EQ(2, readTags(doc).size());
}

TEST_F(FleTagsTest, InsertTwoDifferentAndDeleteOne) {
    auto doc1 = BSON("encrypted" << "a");
    auto doc2 = BSON("encrypted" << "b");

    doSingleInsert(1, doc1);
    doSingleInsert(2, doc2);
    doSingleDelete(1, Fle2AlgorithmInt::kEquality);

    ASSERT_EQ(1, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, InsertAndUpdate) {
    auto doc1 = BSON("encrypted" << "a");
    auto doc2 = BSON("encrypted" << "b");

    doSingleInsert(1, doc1);
    doSingleUpdate(1, doc2);

    // In v2, readTags returns 1 tag for doc1 because stale tags are no longer removed.
    ASSERT_EQ(1, readTags(doc1).size());
    ASSERT_EQ(1, readTags(doc2).size());
}

TEST_F(FleTagsTest, ContentionFactor) {
    auto efc = EncryptedFieldConfig::parse(fromjson(R"({
        "escCollection": "enxcol_.coll.esc",
        "ecocCollection": "enxcol_.coll.ecoc",
        "fields": [{
            "keyId": { "$uuid": "12345678-1234-9876-1234-123456789012"},
            "path": "encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality", "contention": NumberLong(4)}
        }]
    })"),
                                           IDLParserContext("root"));

    auto doc1 = BSON("encrypted" << "a");
    auto doc2 = BSON("encrypted" << "b");

    // Insert doc1 twice with a contention factor of 0 and once with a contention factor or 3.
    doSingleInsertWithContention(1, doc1, 4, 0, efc);
    doSingleInsertWithContention(4, doc1, 4, 3, efc);
    doSingleInsertWithContention(5, doc1, 4, 0, efc);

    // Insert doc2 once with a contention factor of 2 and once with a contention factor of 3.
    doSingleInsertWithContention(7, doc2, 4, 2, efc);
    doSingleInsertWithContention(8, doc2, 4, 3, efc);

    {
        // Test the counts of the results from individual contention factors, ensuring that
        // the data stored on disk and the getTags algorithm is working correctly.
        //
        // This relies on the order preserving nature of the query.

        auto countInfoSetDoc1 = getCountInfoSets(doc1, 4);
        {
            ASSERT_EQ(1, countInfoSetDoc1.size());

            auto countInfoSet = countInfoSetDoc1[0];

            ASSERT_EQ(5, countInfoSet.size());

            ASSERT_EQ(2, countInfoSet[0].count);
            ASSERT_EQ(0, countInfoSet[1].count);
            ASSERT_EQ(0, countInfoSet[2].count);
            ASSERT_EQ(1, countInfoSet[3].count);
        }

        auto countInfoSetDoc2 = getCountInfoSets(doc2, 4);
        {
            ASSERT_EQ(1, countInfoSetDoc2.size());

            auto countInfoSet = countInfoSetDoc2[0];

            ASSERT_EQ(5, countInfoSet.size());

            ASSERT_EQ(0, countInfoSet[0].count);
            ASSERT_EQ(0, countInfoSet[1].count);
            ASSERT_EQ(1, countInfoSet[2].count);
            ASSERT_EQ(1, countInfoSet[3].count);
        }
    }

    ASSERT_EQ(3, readTags(doc1, 4).size());
    ASSERT_EQ(2, readTags(doc2, 4).size());
}

TEST_F(FleTagsTest, MemoryLimit) {
    auto doc = BSON("encrypted" << "a");

    const auto tagLimit = 10;

    // Set memory limit to 10 tags * 40 bytes per tag
    const auto oldLimit = internalQueryFLERewriteMemoryLimit.swap(tagLimit * 40);

    // Do 10 inserts
    for (auto i = 0; i < tagLimit; i++) {
        doSingleInsert(i, doc);
    }

    // readTags returns 10 tags which does not exceed memory limit.
    ASSERT_EQ(tagLimit, readTags(doc).size());

    doSingleInsert(10, doc);

    // readTags returns 11 tags which does exceed memory limit.
    ASSERT_THROWS_CODE(readTags(doc), DBException, ErrorCodes::FLEMaxTagLimitExceeded);

    doSingleDelete(5, Fle2AlgorithmInt::kEquality);

    // readTags returns 11 tags which does exceed memory limit.
    ASSERT_THROWS_CODE(readTags(doc), DBException, ErrorCodes::FLEMaxTagLimitExceeded);

    internalQueryFLERewriteMemoryLimit.store(oldLimit);
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

class QETextSearchCrudTest : public FleCrudTest {
public:
    struct TextSearchSchema {
        QueryTypeEnum type;
        uint32_t lb;
        uint32_t ub;
        uint32_t mlen;
        bool casef;
        bool diacf;
    };

protected:
    void setUp() override {
        FleCrudTest::setUp();
        _ffctrl.emplace("featureFlagQETextSearchPreview", true);
    }

    void tearDown() override {
        FleCrudTest::tearDown();
    }

    // Note - we don't do any validation here; we assume none of the schemas added conflict.
    void addSchema(TextSearchSchema schema) {
        _schemas.push_back(std::move(schema));
    }

    EncryptedFieldConfig getEFC();
    BSONObj generateInsertSpec(BSONElement value);
    std::vector<char> generatePlaceholder(BSONElement value);

    void doInsert(int id, BSONElement element);

    PrfBlock getTestESCDataToken(BSONElement value, boost::optional<QueryTypeEnum> type);
    PrfBlock getTestEDCDataToken(BSONElement value, boost::optional<QueryTypeEnum> type);

    ESCTwiceDerivedTagToken getTestESCTwiceDerivedToken(BSONElement value,
                                                        boost::optional<QueryTypeEnum> type);
    EDCTwiceDerivedToken getTestEDCTwiceDerivedToken(BSONElement value,
                                                     boost::optional<QueryTypeEnum> type);

    BSONObj findESCNonAnchor(BSONElement element,
                             uint64_t cpos,
                             boost::optional<QueryTypeEnum> qtype);

    void verifyESCEntriesForString(StringData testString,
                                   uint32_t expectedCount,
                                   boost::optional<QueryTypeEnum> type = boost::none,
                                   bool padding = false);

    std::vector<PrfBlock> readTags(BSONElement value, boost::optional<QueryTypeEnum> type) {
        return mongo::fle::readTags(_queryImpl.get(),
                                    _escNs,
                                    ESCDerivedFromDataToken{getTestESCDataToken(value, type)},
                                    EDCDerivedFromDataToken{getTestEDCDataToken(value, type)},
                                    0);
    }

    stdx::unordered_set<std::string> getExpectedSubstrings(const unicode::String& foldedString,
                                                           uint32_t lb,
                                                           uint32_t ub);
    stdx::unordered_set<std::string> getExpectedSuffixes(const unicode::String& foldedString,
                                                         uint32_t lb,
                                                         uint32_t ub);
    stdx::unordered_set<std::string> getExpectedPrefixes(const unicode::String& foldedString,
                                                         uint32_t lb,
                                                         uint32_t ub);
    void verifyExpectationsAfterInsertions(
        const std::vector<std::pair<StringData, StringData>>& inserted);

    /**
     * Given an array of unfolded & folded string pairs, inserts each unfolded string
     * as the value of kTestFieldName, which is an indexed-encrypted field utilizing the current
     * substring/suffix/prefix encryption schemas. After each insertion, the EDC/ESC/ECOC are
     * verified to have the correct number of entries.
     */
    void doInsertsAndVerifyExpectations(
        const std::vector<std::pair<StringData, StringData>>& inserts);

    static constexpr StringData kTestFieldName = "field"_sd;
    std::vector<TextSearchSchema> _schemas;
    StackBufBuilder _stackBuf;
    boost::optional<RAIIServerParameterControllerForTest> _ffctrl;
};

EncryptedFieldConfig QETextSearchCrudTest::getEFC() {
    std::vector<QueryTypeConfig> qtcs;
    for (const auto& schema : _schemas) {
        QueryTypeConfig qtc;
        qtc.setQueryType(schema.type);
        if (schema.type == QueryTypeEnum::SubstringPreview) {
            qtc.setStrMaxLength(schema.mlen);
        }
        qtc.setStrMinQueryLength(schema.lb);
        qtc.setStrMaxQueryLength(schema.ub);
        qtc.setCaseSensitive(!schema.casef);
        qtc.setDiacriticSensitive(!schema.diacf);
        qtcs.push_back(std::move(qtc));
    }
    EncryptedField ef(UUID::gen(), std::string{kTestFieldName});
    std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig> vqtcs = std::move(qtcs);
    ef.setBsonType("string"_sd);
    ef.setQueries(vqtcs);
    EncryptedFieldConfig efc({std::move(ef)});
    efc.setEscCollection(_escNs.coll());
    efc.setEcocCollection(_ecocNs.coll());
    efc.setStrEncodeVersion(1);
    return efc;
}

BSONObj QETextSearchCrudTest::generateInsertSpec(BSONElement value) {
    FLE2TextSearchInsertSpec spec;
    spec.setValue(value.String());
    spec.setCaseFold(_schemas[0].casef);
    spec.setDiacriticFold(_schemas[0].diacf);
    for (const auto& schema : _schemas) {
        switch (schema.type) {
            case QueryTypeEnum::SubstringPreview:
                spec.setSubstringSpec(FLE2SubstringInsertSpec(schema.mlen, schema.ub, schema.lb));
                break;
            case QueryTypeEnum::SuffixPreview:
                spec.setSuffixSpec(FLE2SuffixInsertSpec(schema.ub, schema.lb));
                break;
            case QueryTypeEnum::PrefixPreview:
                spec.setPrefixSpec(FLE2PrefixInsertSpec(schema.ub, schema.lb));
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
    return BSON("s" << spec.toBSON());
}

std::vector<char> QETextSearchCrudTest::generatePlaceholder(BSONElement value) {
    FLE2EncryptionPlaceholder ep;
    ep.setAlgorithm(Fle2AlgorithmInt::kTextSearch);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);

    BSONObj spec = generateInsertSpec(value);
    ep.setValue(spec.firstElement());

    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

void QETextSearchCrudTest::doInsert(int id, BSONElement element) {
    auto buf = generatePlaceholder(element);
    auto efc = getEFC();

    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.append(transformElementForInsertUpdate(element, buf, efc).firstElement());
    auto result = builder.obj();

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);
    uassertStatusOK(processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false));
}

PrfBlock QETextSearchCrudTest::getTestESCDataToken(BSONElement element,
                                                   boost::optional<QueryTypeEnum> type) {
    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = ESCToken::deriveFrom(c1token);
    if (!type.has_value()) {
        auto escTextToken = ESCTextExactToken::deriveFrom(escToken);
        return ESCTextExactDerivedFromDataToken::deriveFrom(escTextToken, toCDR(element))
            .asPrfBlock();
    }
    switch (*type) {
        case QueryTypeEnum::SubstringPreview: {
            auto escTextToken = ESCTextSubstringToken::deriveFrom(escToken);
            return ESCTextSubstringDerivedFromDataToken::deriveFrom(escTextToken, toCDR(element))
                .asPrfBlock();
        }
        case QueryTypeEnum::SuffixPreview: {
            auto escTextToken = ESCTextSuffixToken::deriveFrom(escToken);
            return ESCTextSuffixDerivedFromDataToken::deriveFrom(escTextToken, toCDR(element))
                .asPrfBlock();
        }
        case QueryTypeEnum::PrefixPreview: {
            auto escTextToken = ESCTextPrefixToken::deriveFrom(escToken);
            return ESCTextPrefixDerivedFromDataToken::deriveFrom(escTextToken, toCDR(element))
                .asPrfBlock();
        }
        default:
            MONGO_UNREACHABLE;
    }
}

PrfBlock QETextSearchCrudTest::getTestEDCDataToken(BSONElement element,
                                                   boost::optional<QueryTypeEnum> type) {
    auto c1token = CollectionsLevel1Token::deriveFrom(_keyVault.getIndexKeyById(indexKeyId).key);
    auto edcToken = EDCToken::deriveFrom(c1token);
    if (!type.has_value()) {
        auto edcTextToken = EDCTextExactToken::deriveFrom(edcToken);
        return EDCTextExactDerivedFromDataToken::deriveFrom(edcTextToken, toCDR(element))
            .asPrfBlock();
    }
    switch (*type) {
        case QueryTypeEnum::SubstringPreview: {
            auto edcTextToken = EDCTextSubstringToken::deriveFrom(edcToken);
            return EDCTextSubstringDerivedFromDataToken::deriveFrom(edcTextToken, toCDR(element))
                .asPrfBlock();
        }
        case QueryTypeEnum::SuffixPreview: {
            auto edcTextToken = EDCTextSuffixToken::deriveFrom(edcToken);
            return EDCTextSuffixDerivedFromDataToken::deriveFrom(edcTextToken, toCDR(element))
                .asPrfBlock();
        }
        case QueryTypeEnum::PrefixPreview: {
            auto edcTextToken = EDCTextPrefixToken::deriveFrom(edcToken);
            return EDCTextPrefixDerivedFromDataToken::deriveFrom(edcTextToken, toCDR(element))
                .asPrfBlock();
        }
        default:
            MONGO_UNREACHABLE;
    }
}

ESCTwiceDerivedTagToken QETextSearchCrudTest::getTestESCTwiceDerivedToken(
    BSONElement element, boost::optional<QueryTypeEnum> type) {
    PrfBlock dataTokenBlk = getTestESCDataToken(element, type);
    PrfBlock cfTokenBlk;

    if (!type.has_value()) {
        cfTokenBlk = ESCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                         ESCTextExactDerivedFromDataToken{dataTokenBlk}, 0)
                         .asPrfBlock();
    } else {
        switch (*type) {
            case QueryTypeEnum::SubstringPreview: {
                cfTokenBlk =
                    ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                        ESCTextSubstringDerivedFromDataToken{dataTokenBlk}, 0)
                        .asPrfBlock();
                break;
            }
            case QueryTypeEnum::SuffixPreview: {
                cfTokenBlk = ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                                 ESCTextSuffixDerivedFromDataToken{dataTokenBlk}, 0)
                                 .asPrfBlock();
                break;
            }
            case QueryTypeEnum::PrefixPreview: {
                cfTokenBlk = ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                                 ESCTextPrefixDerivedFromDataToken{dataTokenBlk}, 0)
                                 .asPrfBlock();
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return ESCTwiceDerivedTagToken::deriveFrom(
        ESCDerivedFromDataTokenAndContentionFactorToken{cfTokenBlk});
}

EDCTwiceDerivedToken QETextSearchCrudTest::getTestEDCTwiceDerivedToken(
    BSONElement element, boost::optional<QueryTypeEnum> type) {
    PrfBlock dataTokenBlk = getTestEDCDataToken(element, type);
    PrfBlock cfTokenBlk;

    if (!type.has_value()) {
        cfTokenBlk = EDCTextExactDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                         EDCTextExactDerivedFromDataToken{dataTokenBlk}, 0)
                         .asPrfBlock();
    } else {
        switch (*type) {
            case QueryTypeEnum::SubstringPreview: {
                cfTokenBlk =
                    EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                        EDCTextSubstringDerivedFromDataToken{dataTokenBlk}, 0)
                        .asPrfBlock();
                break;
            }
            case QueryTypeEnum::SuffixPreview: {
                cfTokenBlk = EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                                 EDCTextSuffixDerivedFromDataToken{dataTokenBlk}, 0)
                                 .asPrfBlock();
                break;
            }
            case QueryTypeEnum::PrefixPreview: {
                cfTokenBlk = EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken::deriveFrom(
                                 EDCTextPrefixDerivedFromDataToken{dataTokenBlk}, 0)
                                 .asPrfBlock();
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return EDCTwiceDerivedToken::deriveFrom(
        EDCDerivedFromDataTokenAndContentionFactorToken{cfTokenBlk});
}

BSONObj QETextSearchCrudTest::findESCNonAnchor(BSONElement element,
                                               uint64_t cpos,
                                               boost::optional<QueryTypeEnum> qtype) {
    return _queryImpl->getById(_escNs,
                               ESCCollection::generateNonAnchorId(
                                   &hmacCtx, getTestESCTwiceDerivedToken(element, qtype), cpos));
}

void QETextSearchCrudTest::verifyESCEntriesForString(StringData testString,
                                                     uint32_t expectedCount,
                                                     boost::optional<QueryTypeEnum> qtype,
                                                     bool padding) {
    auto doc = BSON(kTestFieldName << testString);
    if (padding) {
        ASSERT(qtype.has_value());  // padding values must always have a query type
        doc = BSON(kTestFieldName << (testString + "\xff"));
    }
    auto element = doc.firstElement();

    // check expected number of matching ESC entries were inserted
    for (uint32_t ct = 1; ct <= expectedCount; ct++) {
        ASSERT_FALSE(findESCNonAnchor(element, ct, qtype).isEmpty())
            << "No ESC entry found for string='" << testString << "' count=" << ct;
    }
    ASSERT_TRUE(findESCNonAnchor(element, expectedCount + 1, qtype).isEmpty())
        << "Unexpected ESC entry found for string='" << testString
        << "' count=" << (expectedCount + 1);
    ;

    // check readTags returns correct number tags
    std::vector<PrfBlock> tags;
    try {
        tags = readTags(element, qtype);
    } catch (DBException& ex) {
        ASSERT_EQ(ex.toStatus().code(), ErrorCodes::FLEMaxTagLimitExceeded);
        return;
    }
    ASSERT_EQ(tags.size(), expectedCount);

    // check the read tags are as expected
    HmacContext hmacCtx;
    auto edcTwiceDerivedToken = getTestEDCTwiceDerivedToken(element, qtype);
    for (uint32_t i = 1; i <= expectedCount; i++) {
        auto tagToFind = EDCServerCollection::generateTag(&hmacCtx, edcTwiceDerivedToken, i);
        ASSERT_NE(tags.end(), std::find(tags.begin(), tags.end(), tagToFind))
            << "readTags output missing tag for count=" << i << " total=" << expectedCount;
    }
}

stdx::unordered_set<std::string> QETextSearchCrudTest::getExpectedSubstrings(
    const unicode::String& foldedString, uint32_t lb, uint32_t ub) {
    stdx::unordered_set<std::string> res;
    for (uint32_t ss_len = lb; ss_len <= std::min(ub, uint32_t(foldedString.size())); ss_len++) {
        for (uint32_t i = 0; i <= foldedString.size() - ss_len; i++) {
            res.insert(std::string{foldedString.substrToBuf(&_stackBuf, i, ss_len)});
        }
    }
    return res;
}

stdx::unordered_set<std::string> QETextSearchCrudTest::getExpectedSuffixes(
    const unicode::String& foldedString, uint32_t lb, uint32_t ub) {
    stdx::unordered_set<std::string> res;
    for (uint32_t suff_len = lb; suff_len <= std::min(ub, uint32_t(foldedString.size()));
         suff_len++) {
        res.insert(std::string{
            foldedString.substrToBuf(&_stackBuf, foldedString.size() - suff_len, suff_len)});
    }
    return res;
}

stdx::unordered_set<std::string> QETextSearchCrudTest::getExpectedPrefixes(
    const unicode::String& foldedString, uint32_t lb, uint32_t ub) {
    stdx::unordered_set<std::string> res;
    for (uint32_t pref_len = lb; pref_len <= std::min(ub, uint32_t(foldedString.size()));
         pref_len++) {
        res.insert(std::string{foldedString.substrToBuf(&_stackBuf, 0, pref_len)});
    }
    return res;
}

void QETextSearchCrudTest::verifyExpectationsAfterInsertions(
    const std::vector<std::pair<StringData, StringData>>& inserted) {
    stdx::unordered_map<std::string, int> affixCounts[3];
    stdx::unordered_map<std::string, int> exactCounts;
    stdx::unordered_map<std::string, uint32_t> paddingCounts[3];
    uint32_t totalTags = 0;

    auto queryTypeToIndex = [](QueryTypeEnum qt) -> uint8_t {
        switch (qt) {
            case QueryTypeEnum::SubstringPreview:
                return 0;
            case QueryTypeEnum::SuffixPreview:
                return 1;
            case QueryTypeEnum::PrefixPreview:
                return 2;
            default:
                MONGO_UNREACHABLE;
        }
    };

    for (const auto& [unfoldedStr, foldedStr] : inserted) {
        if (_schemas.empty()) {
            continue;
        }

        totalTags++;  // exact
        unicode::String unicodeFoldedStr(foldedStr);
        std::string foldedStrStd{foldedStr};
        exactCounts[foldedStrStd]++;
        for (const auto& schema : _schemas) {
            uint32_t msize;
            stdx::unordered_set<std::string> affixes;
            switch (schema.type) {
                case QueryTypeEnum::SubstringPreview:
                    msize =
                        msizeForSubstring(unfoldedStr.size(), schema.lb, schema.ub, schema.mlen);
                    affixes = getExpectedSubstrings(unicodeFoldedStr, schema.lb, schema.ub);
                    break;
                case QueryTypeEnum::SuffixPreview:
                    msize = msizeForSuffixOrPrefix(unfoldedStr.size(), schema.lb, schema.ub);
                    affixes = getExpectedSuffixes(unicodeFoldedStr, schema.lb, schema.ub);
                    break;
                case QueryTypeEnum::PrefixPreview:
                    msize = msizeForSuffixOrPrefix(unfoldedStr.size(), schema.lb, schema.ub);
                    affixes = getExpectedPrefixes(unicodeFoldedStr, schema.lb, schema.ub);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }

            auto qt_index = queryTypeToIndex(schema.type);
            uint32_t padCount = msize - affixes.size();
            paddingCounts[qt_index][foldedStrStd] += padCount;
            for (const auto& affix : affixes) {
                affixCounts[qt_index][affix]++;
            }
            totalTags += msize;
        }
    }

    for (const auto& [exactStr, count] : exactCounts) {
        verifyESCEntriesForString(exactStr, count);
    }

    for (auto qt : {QueryTypeEnum::SubstringPreview,
                    QueryTypeEnum::SuffixPreview,
                    QueryTypeEnum::PrefixPreview}) {
        auto qt_index = queryTypeToIndex(qt);
        for (const auto& [exactStr, count] : paddingCounts[qt_index]) {
            verifyESCEntriesForString(exactStr, count, qt, true /*padding*/);
        }
        for (const auto& [affix, count] : affixCounts[qt_index]) {
            verifyESCEntriesForString(affix, count, qt);
        }
    }

    assertDocumentCounts(inserted.size(), totalTags, totalTags);
    assertECOCDocumentCountByField(kTestFieldName, totalTags);
}

void QETextSearchCrudTest::doInsertsAndVerifyExpectations(
    const std::vector<std::pair<StringData, StringData>>& inserts) {
    std::vector<std::pair<StringData, StringData>> inserted;
    verifyExpectationsAfterInsertions(inserted);

    for (size_t i = 0; i < inserts.size(); i++) {
        auto toInsert = BSON(kTestFieldName << inserts[i].first);
        doInsert(i + 1, toInsert.firstElement());
        inserted.push_back(inserts[i]);
        verifyExpectationsAfterInsertions(inserted);
    }
}

TEST_F(QETextSearchCrudTest, BasicSubstring) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 10,
               .ub = 100,
               .mlen = 1000,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"}});
}

TEST_F(QETextSearchCrudTest, BasicSuffix) {
    addSchema({.type = QueryTypeEnum::SuffixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"}});
}

TEST_F(QETextSearchCrudTest, BasicPrefix) {
    addSchema({.type = QueryTypeEnum::PrefixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"}});
}

TEST_F(QETextSearchCrudTest, BasicPrefixAndSuffix) {
    addSchema({.type = QueryTypeEnum::SuffixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    addSchema({.type = QueryTypeEnum::PrefixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"}});
}

TEST_F(QETextSearchCrudTest, RepeatingSubstring) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 10,
               .ub = 100,
               .mlen = 1000,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"aaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaa"}});
}

TEST_F(QETextSearchCrudTest, FoldAsciiSuffix) {
    addSchema({.type = QueryTypeEnum::SuffixPreview,
               .lb = 10,
               .ub = 100,
               .mlen = 1000,
               .casef = true,
               .diacf = true});
    doInsertsAndVerifyExpectations(
        {{"D^e`````^^M^o^^^^n``st```rA^^`^`^`^tI``on", "demonstration"}});
}

TEST_F(QETextSearchCrudTest, UnicodeSubstring) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 1,
               .ub = 5,
               .mlen = 1000,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"私はぺんです.", "私はぺんです."}});
}

TEST_F(QETextSearchCrudTest, FoldUnicodeSubstring) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 3,
               .ub = 5,
               .mlen = 1000,
               .casef = true,
               .diacf = true});
    doInsertsAndVerifyExpectations(
        {{"私\xf0\x90\xb4\xa5はẂ𑂚^ぺ\xcc\x86\xcc\x86ん\xef\xad\x83`.𓀈\xf0\x90\xb4\xa5𓀍`AЃ",
          "私はw𑂙へん\xd7\xa3.𓀈𓀍aг"}});
}

TEST_F(QETextSearchCrudTest, BasicSubstringMultipleInserts) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 10,
               .ub = 100,
               .mlen = 1000,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"},
                                    {"hello to the world", "hello to the world"},
                                    {"goodbye to the world", "goodbye to the world"},
                                    {"demonstration", "demonstration"},
                                    {"aaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaa"},
                                    {"aaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaa"}});
}

TEST_F(QETextSearchCrudTest, BasicSuffixMultipleInserts) {
    addSchema({.type = QueryTypeEnum::SuffixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"},
                                    {"hello to the world", "hello to the world"},
                                    {"goodbye to the world", "goodbye to the world"},
                                    {"demonstration", "demonstration"},
                                    {"aaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaa"},
                                    {"aaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaa"}});
}

TEST_F(QETextSearchCrudTest, BasicPrefixMultipleInserts) {
    addSchema({.type = QueryTypeEnum::PrefixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"},
                                    {"hello to the world", "hello to the world"},
                                    {"goodbye to the world", "goodbye to the world"},
                                    {"demonstration", "demonstration"},
                                    {"aaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaa"},
                                    {"aaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaa"}});
}

TEST_F(QETextSearchCrudTest, BasicPrefixAndSuffixMultipleInserts) {
    addSchema({.type = QueryTypeEnum::SuffixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    addSchema({.type = QueryTypeEnum::PrefixPreview,
               .lb = 10,
               .ub = 100,
               .casef = false,
               .diacf = false});
    doInsertsAndVerifyExpectations({{"demonstration", "demonstration"},
                                    {"hello to the world", "hello to the world"},
                                    {"goodbye to the world", "goodbye to the world"},
                                    {"demonstration", "demonstration"},
                                    {"aaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaa"},
                                    {"aaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaa"}});
}

// Test insert update payloads containing text search token sets ('b') with embedded encryptedTokens
// of invalid length in the exact/substring/prefix/suffix token sets are rejected.
TEST_F(QETextSearchCrudTest, InsertPayloadHasInvalidExactEncryptedTokensForTextSearch) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 2,
               .ub = 10,
               .mlen = 400,
               .casef = false,
               .diacf = false});
    auto doc = BSON(kTestFieldName << "abcdef");
    auto element = doc.firstElement();
    auto buf = generatePlaceholder(element);
    auto efc = getEFC();

    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.append(transformElementForInsertUpdate(element, buf, efc).firstElement());
    auto result = builder.obj();

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);
    ASSERT_EQ(serverPayload.size(), 1);
    auto& exactSet = serverPayload[0].payload.getTextSearchTokenSets().value().getExactTokenSet();
    exactSet.setEncryptedTokens(
        StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                    exactSet.getEscDerivedToken().asPrfBlock()),
                                boost::none,
                                boost::none /* msize */)
            .encrypt({{}}));
    ASSERT_THROWS_CODE_AND_WHAT(
        processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false),
        DBException,
        ErrorCodes::BadValue,
        "Invalid length for EncryptedStateCollectionTokensV2 for text "
        "search: Expected 51, got 48");
}

TEST_F(QETextSearchCrudTest, InsertPayloadHasInvalidSubstringEncryptedTokensForTextSearch) {
    addSchema({.type = QueryTypeEnum::SubstringPreview,
               .lb = 2,
               .ub = 10,
               .mlen = 400,
               .casef = false,
               .diacf = false});
    auto doc = BSON(kTestFieldName << "abcdef");
    auto element = doc.firstElement();
    auto buf = generatePlaceholder(element);
    auto efc = getEFC();

    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.append(transformElementForInsertUpdate(element, buf, efc).firstElement());
    auto result = builder.obj();

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);
    ASSERT_EQ(serverPayload.size(), 1);
    auto& ts =
        serverPayload[0].payload.getTextSearchTokenSets().value().getSubstringTokenSets().back();
    ts.setEncryptedTokens(StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken(
                                                      ts.getEscDerivedToken().asPrfBlock()),
                                                  boost::none,
                                                  boost::none /* msize */)
                              .encrypt({{}}));
    ASSERT_THROWS_CODE_AND_WHAT(
        processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, 0, result, false),
        DBException,
        ErrorCodes::BadValue,
        "Invalid length for EncryptedStateCollectionTokensV2 for text "
        "search: Expected 51, got 48");
}

}  // namespace
}  // namespace mongo

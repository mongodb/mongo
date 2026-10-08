// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor.h"

#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/validate/key_string_index_consistency.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <vector>

namespace mongo::collection_validation {
using namespace std::literals::string_view_literals;
namespace {

const auto kNss = NamespaceString::createNamespaceString_forTest("validateAdaptorDB.coll"sv);

class ValidateAdaptorTest : public CatalogTestFixture {
protected:
    struct Result {
        ValidateAdaptor::ValidateRecordResult record;
        ValidateResults results;
    };

    void createCollection(const CollectionOptions& options, const std::vector<BSONObj>& indexes) {
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, options));
        if (!indexes.empty()) {
            ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
                operationContext(), kNss, indexes));
        }
    }

    Result validateRecord(const std::vector<char>& data, ValidateMode mode) {
        auto opCtx = operationContext();
        ValidateState validateState(
            opCtx, kNss, ValidationOptions(mode, RepairMode::kNone, /*logDiagnostics=*/false));
        ASSERT_OK(validateState.initializeCollection(opCtx));
        validateState.initializeCursors(opCtx);

        const auto& coll = validateState.getCollection();
        std::vector<const IndexCatalogEntry*> indexEntries;
        for (const auto& indexIdent : validateState.getIndexIdents()) {
            indexEntries.push_back(coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent));
        }

        ValidateAdaptor adaptor(opCtx, &validateState);
        KeyStringIndexConsistency keyStringIndexConsistency(opCtx, &validateState);
        Result result;
        const Record record{RecordId(1), RecordData(data.data(), data.size())};
        result.record = adaptor.validateRecord(
            opCtx, record, result.results, keyStringIndexConsistency, indexEntries);
        return result;
    }
};

std::vector<char> toBuffer(const BSONObj& obj) {
    const char* begin = obj.objdata();
    return {begin, begin + obj.objsize()};
}

// Overwrites the type byte of 'fieldName' with a type that does not exist.
std::vector<char> withUnknownType(const BSONObj& obj, std::string_view fieldName) {
    auto data = toBuffer(obj);
    data[obj[fieldName].rawdata() - obj.objdata()] = 0x7E;
    return data;
}

// Overwrites the length prefix of the string 'fieldName' so that it extends far past the buffer.
std::vector<char> withOversizedStringLength(const BSONObj& obj, std::string_view fieldName) {
    auto data = toBuffer(obj);
    DataView(data.data() + (obj[fieldName].value() - obj.objdata()))
        .write<LittleEndian<int32_t>>(0x7FFFFFF0);
    return data;
}

BSONObj withInvalidUTF8(std::string_view fieldName) {
    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append(fieldName, "z\xff"sv);
    builder.append("b", 1);
    return builder.obj();
}

TEST_F(ValidateAdaptorTest, ConformantDocumentIsValid) {
    createCollection({}, {});
    const auto result = validateRecord(toBuffer(BSON("_id" << 1 << "a" << "z")),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_OK(result.record.status);
    EXPECT_TRUE(result.record.compliantDocument);
    EXPECT_TRUE(result.record.validDocument);
}

TEST_F(ValidateAdaptorTest, NonConformantDocumentIsCompliantFalse) {
    createCollection({}, {});
    const auto result =
        validateRecord(toBuffer(withInvalidUTF8("a")), ValidateMode::kForegroundCheckBSON);
    EXPECT_OK(result.record.status);
    EXPECT_FALSE(result.record.compliantDocument);
}

TEST_F(ValidateAdaptorTest, InvalidBSONAfterNonConformantElementIsReported) {
    createCollection({}, {});
    const auto result = validateRecord(withUnknownType(withInvalidUTF8("a"), "b"),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_EQ(result.record.status.code(), ErrorCodes::InvalidBSON);
}

TEST_F(ValidateAdaptorTest, InvalidBSONAfterNonConformantElementIsReportedWithPartialIndex) {
    createCollection({},
                     {BSON("v" << 2                        //
                               << "name" << "b_1"          //
                               << "key" << BSON("b" << 1)  //
                               << "partialFilterExpression" << BSON("b" << BSON("$gt" << 0)))});
    const auto result = validateRecord(withUnknownType(withInvalidUTF8("a"), "b"),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_EQ(result.record.status.code(), ErrorCodes::InvalidBSON);
}

TEST_F(ValidateAdaptorTest, OversizedStringAfterNonConformantElementIsReported) {
    createCollection({},
                     {BSON("v" << 2                //
                               << "name" << "b_1"  //
                               << "key" << BSON("b" << 1))});
    BSONObjBuilder builder;
    builder.append("_id", 1);
    builder.append("a", "z\xff"sv);
    builder.append("b", "x");
    const auto result = validateRecord(withOversizedStringLength(builder.obj(), "b"),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_EQ(result.record.status.code(), ErrorCodes::InvalidBSON);
}

TEST_F(ValidateAdaptorTest, InvalidUTF8InCollatedPartialIndexFieldDoesNotThrow) {
    CollectionOptions options;
    options.collation = BSON("locale" << "en_US");
    createCollection(options,
                     {BSON("v" << 2                //
                               << "name" << "a_1"  //
                               << "key" << BSON("a" << 1) << "partialFilterExpression"
                               << BSON("a" << BSON("$gt" << "m")))});
    const auto result =
        validateRecord(toBuffer(withInvalidUTF8("a")), ValidateMode::kForegroundCheckBSON);
    EXPECT_OK(result.record.status);
    EXPECT_FALSE(result.record.compliantDocument);
}

}  // namespace
}  // namespace mongo::collection_validation

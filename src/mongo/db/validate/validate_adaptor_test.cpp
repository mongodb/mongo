// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor.h"

#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/validate/key_string_index_consistency.h"
#include "mongo/db/validate/validate_adaptor_factory.h"
#include "mongo/db/validate/validate_adaptor_parallel.h"
#include "mongo/db/validate/validate_adaptor_serial.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::collection_validation {
using namespace std::literals::string_view_literals;
namespace {

/**
 * A cursor that replays scripted RecordIds regardless of what the record store holds, so that a
 * record store that returns records out of order can be simulated. seek() replays the sequence
 * scripted for the RecordId it is given, and next() continues through it until it runs out.
 */
class ScriptedValidateCursor final : public ValidateCursor {
public:
    using Script = std::map<int64_t, std::vector<int64_t>>;

    explicit ScriptedValidateCursor(const Script& script) : _script(script) {}

    boost::optional<Record> seek(const RecordId& recordId) override {
        const auto it = _script.find(recordId.getLong());
        uassert(
            ErrorCodes::InternalError, "No sequence scripted for the seek", it != _script.end());
        _sequence = &it->second;
        _position = 0;
        return current();
    }
    boost::optional<Record> next() override {
        ++_position;
        return current();
    }
    void yield() override {}

private:
    boost::optional<Record> current() {
        if (_position >= _sequence->size()) {
            return boost::none;
        }
        const auto id = (*_sequence)[_position];
        _doc = BSON("_id" << id);
        return Record{RecordId(id), RecordData(_doc.objdata(), _doc.objsize())};
    }

    const Script& _script;
    const std::vector<int64_t>* _sequence{nullptr};
    size_t _position{0};
    BSONObj _doc;
};

class ScriptedSerialAdaptor final : public ValidateAdaptor {
public:
    ScriptedSerialAdaptor(OperationContext* opCtx,
                          ValidateState& state,
                          const ScriptedValidateCursor::Script& script)
        : ValidateAdaptor(opCtx, state), _script(script) {}

    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults& validateResults,
                             ValidationVersion validationVersion) final {
        const auto progressGuard = preTraverseRecordStoreImpl(opCtx);
        auto impl = traverseRecordStoreImpl(
            opCtx,
            validateResults,
            getKeyBasedIndexConsistency(),
            {.beginRecordId = getValidateState().getFirstRecordId(),
             .cursorFactory = [this](OperationContext*) -> std::unique_ptr<ValidateCursor> {
                 return std::make_unique<ScriptedValidateCursor>(_script);
             }},
            getProgress(),
            validationVersion);
        validateResults = std::move(impl.validateResults);
    }

private:
    const ScriptedValidateCursor::Script& _script;
};

class ScriptedParallelAdaptor final : public ValidateAdaptorParallel {
public:
    ScriptedParallelAdaptor(OperationContext* opCtx,
                            ValidateState& state,
                            int64_t targetRecordsPerSlice,
                            const ScriptedValidateCursor::Script& script)
        : ValidateAdaptorParallel(opCtx, state, targetRecordsPerSlice), _script(script) {}

protected:
    std::unique_ptr<ValidateCursor> makeSliceCursor(OperationContext*,
                                                    const RecordStore&,
                                                    CancellationToken) const override {
        return std::make_unique<ScriptedValidateCursor>(_script);
    }

private:
    const ScriptedValidateCursor::Script& _script;
};

const auto kNss = NamespaceString::createNamespaceString_forTest("validateAdaptorDB.coll"sv);

enum class AdaptorKind { kSerial, kParallel };

class ValidateAdaptorTest : public CatalogTestFixture,
                            public testing::WithParamInterface<AdaptorKind> {
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
        // The factory selects the parallel adaptor when a slice target is set.
        const boost::optional<int64_t> targetRecordsPerSlice =
            GetParam() == AdaptorKind::kParallel ? boost::optional<int64_t>(10) : boost::none;
        ValidateState validateState(opCtx,
                                    kNss,
                                    ValidationOptions(mode,
                                                      RepairMode::kNone,
                                                      /*logDiagnostics=*/false,
                                                      currentValidationVersion,
                                                      /*verifyConfigurationOverride=*/boost::none,
                                                      /*readTimestamp=*/boost::none,
                                                      /*hashPrefixes=*/boost::none,
                                                      /*revealHashedIds=*/boost::none,
                                                      targetRecordsPerSlice));
        ASSERT_OK(validateState.initializeCollection(opCtx));
        validateState.initializeCursors(opCtx);

        const auto& coll = validateState.getCollection();
        std::vector<const IndexCatalogEntry*> indexEntries;
        for (const auto& indexIdent : validateState.getIndexIdents()) {
            indexEntries.push_back(coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent));
        }

        auto adaptor = ValidateAdaptorFactory::create(opCtx, validateState);
        if (GetParam() == AdaptorKind::kParallel) {
            ASSERT_TRUE(dynamic_cast<ValidateAdaptorParallel*>(adaptor.get()));
        } else {
            ASSERT_TRUE(dynamic_cast<ValidateAdaptorSerial*>(adaptor.get()));
        }
        KeyStringIndexConsistency keyStringIndexConsistency(opCtx, &validateState);
        Result result;
        const Record record{RecordId(1), RecordData(data.data(), data.size())};
        result.record = adaptor->validateRecord(
            opCtx, record, result.results, keyStringIndexConsistency, indexEntries);
        return result;
    }

    // Seeds the collection with records 1..numRecords, so that a parallel traversal slices it.
    void insertRecords(int64_t numRecords) {
        auto opCtx = operationContext();
        const AutoGetCollection coll(opCtx, kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        for (int64_t id = 1; id <= numRecords; ++id) {
            const auto doc = BSON("_id" << id);
            ASSERT_OK(
                coll->getRecordStore()->insertRecord(opCtx,
                                                     *shard_role_details::getRecoveryUnit(opCtx),
                                                     RecordId(id),
                                                     doc.objdata(),
                                                     doc.objsize(),
                                                     Timestamp()));
        }
        wuow.commit();
    }

    // Runs the adaptor's record store traversal over a record store that is scripted to return
    // 'serialOrder' to a whole-record-store scan, and 'sliceOrders' (keyed by the RecordId each
    // slice begins at) to a parallel one. Both describe the same records in the same order.
    ValidateResults traverseScripted(const std::vector<int64_t>& serialOrder,
                                     const ScriptedValidateCursor::Script& sliceOrders) {
        auto opCtx = operationContext();
        const bool parallel = GetParam() == AdaptorKind::kParallel;
        ValidateState state(
            opCtx,
            kNss,
            ValidationOptions(ValidateMode::kForeground,
                              RepairMode::kNone,
                              /*logDiagnostics=*/false,
                              currentValidationVersion,
                              /*verifyConfigurationOverride=*/boost::none,
                              /*readTimestamp=*/boost::none,
                              /*hashPrefixes=*/boost::none,
                              /*revealHashedIds=*/boost::none,
                              parallel ? boost::optional<int64_t>(kSliceSize) : boost::none));
        ASSERT_OK(state.initializeCollection(opCtx));
        state.initializeCursors(opCtx);

        ValidateResults results;
        results.setNamespaceString(state.nss());
        results.setUUID(state.uuid());
        results.setRepairMode(state.getRepairMode());
        results.setReadTimestamp(state.getReadTimestamp());

        const ScriptedValidateCursor::Script serialScript{{serialOrder.front(), serialOrder}};
        if (parallel) {
            ScriptedParallelAdaptor(opCtx, state, kSliceSize, sliceOrders)
                .traverseRecordStore(opCtx, results, currentValidationVersion);
        } else {
            ScriptedSerialAdaptor(opCtx, state, serialScript)
                .traverseRecordStore(opCtx, results, currentValidationVersion);
        }
        return results;
    }

    // Twenty records at ten per slice make two slices, whose pivots are the first record and the
    // one nine ids on from it.
    static constexpr int64_t kNumRecords{20};
    static constexpr int64_t kSliceSize{10};
    static constexpr int64_t kSecondSliceBegin{10};
};

[[nodiscard]] bool reportsOutOfOrderRecords(const ValidateResults& results) {
    return std::any_of(
        results.getErrors().begin(), results.getErrors().end(), [](const auto& error) {
            return error.find("out-of-order") != std::string::npos;
        });
}

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

TEST_P(ValidateAdaptorTest, ConformantDocumentIsValid) {
    createCollection({}, {});
    const auto result = validateRecord(toBuffer(BSON("_id" << 1 << "a" << "z")),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_OK(result.record.status);
    EXPECT_TRUE(result.record.compliantDocument);
    EXPECT_TRUE(result.record.validDocument);
}

TEST_P(ValidateAdaptorTest, NonConformantDocumentIsCompliantFalse) {
    createCollection({}, {});
    const auto result =
        validateRecord(toBuffer(withInvalidUTF8("a")), ValidateMode::kForegroundCheckBSON);
    EXPECT_OK(result.record.status);
    EXPECT_FALSE(result.record.compliantDocument);
}

TEST_P(ValidateAdaptorTest, InvalidBSONAfterNonConformantElementIsReported) {
    createCollection({}, {});
    const auto result = validateRecord(withUnknownType(withInvalidUTF8("a"), "b"),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_EQ(result.record.status.code(), ErrorCodes::InvalidBSON);
}

TEST_P(ValidateAdaptorTest, InvalidBSONAfterNonConformantElementIsReportedWithPartialIndex) {
    createCollection({},
                     {BSON("v" << 2                        //
                               << "name" << "b_1"          //
                               << "key" << BSON("b" << 1)  //
                               << "partialFilterExpression" << BSON("b" << BSON("$gt" << 0)))});
    const auto result = validateRecord(withUnknownType(withInvalidUTF8("a"), "b"),
                                       ValidateMode::kForegroundCheckBSON);
    EXPECT_EQ(result.record.status.code(), ErrorCodes::InvalidBSON);
}

TEST_P(ValidateAdaptorTest, OversizedStringAfterNonConformantElementIsReported) {
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

TEST_P(ValidateAdaptorTest, InvalidUTF8InCollatedPartialIndexFieldDoesNotThrow) {
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

std::vector<int64_t> range(int64_t first, int64_t last) {
    std::vector<int64_t> ids;
    for (auto id = first; id <= last; ++id) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<int64_t> concat(std::vector<int64_t> a, std::span<const int64_t> b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

TEST_P(ValidateAdaptorTest, InOrderRecordsAreNotReportedOutOfOrder) {
    createCollection({}, {});
    insertRecords(kNumRecords);
    const auto results = traverseScripted(
        range(1, kNumRecords),
        {{1, range(1, kSecondSliceBegin - 1)}, {kSecondSliceBegin, range(kSecondSliceBegin, 20)}});
    EXPECT_FALSE(reportsOutOfOrderRecords(results));
}

TEST_P(ValidateAdaptorTest, OutOfOrderRecordsWithinASliceAreReported) {
    createCollection({}, {});
    insertRecords(kNumRecords);
    const auto results = traverseScripted(concat(range(1, 5), concat({3}, range(6, kNumRecords))),
                                          {{1, concat(range(1, 5), std::vector<int64_t>{3})},
                                           {kSecondSliceBegin, range(kSecondSliceBegin, 20)}});
    EXPECT_TRUE(reportsOutOfOrderRecords(results));
}

// The record store hands back a record the first slice already read as the first record of the
// second one, so that the records are out of order only when the slices are read one after the
// other. Neither slice sees the out-of-order pair on its own.
TEST_P(ValidateAdaptorTest, OutOfOrderRecordsAcrossSliceBoundaryAreReported) {
    createCollection({}, {});
    insertRecords(kNumRecords);
    const auto results = traverseScripted(
        concat(range(1, kSecondSliceBegin - 1), concat({8}, range(kSecondSliceBegin, 20))),
        {{1, range(1, kSecondSliceBegin - 1)},
         {kSecondSliceBegin, concat({8}, range(kSecondSliceBegin, 20))}});
    EXPECT_TRUE(reportsOutOfOrderRecords(results));
}

// Every slice's records are in order and the slices' ranges do not intersect, but the second slice
// reads records that come after the third slice's.
TEST_P(ValidateAdaptorTest, ReversedDisjointSlicesAreReported) {
    createCollection({}, {});
    insertRecords(30);
    const auto results =
        traverseScripted(concat(range(1, 9), concat(range(14, 18), range(12, 13))),
                         {{1, range(1, 9)}, {10, range(14, 18)}, {19, range(12, 13)}});
    EXPECT_TRUE(reportsOutOfOrderRecords(results));
}

INSTANTIATE_TEST_SUITE_P(,
                         ValidateAdaptorTest,
                         testing::Values(AdaptorKind::kSerial, AdaptorKind::kParallel),
                         [](const testing::TestParamInfo<AdaptorKind>& info) {
                             return info.param == AdaptorKind::kSerial ? "Serial" : "Parallel";
                         });

}  // namespace
}  // namespace mongo::collection_validation

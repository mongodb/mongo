// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/write_cmd_shape_registration.h"

#include "mongo/bson/json.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/util/version/releases.h"

namespace mongo::query_stats {
namespace {

using namespace std::literals::string_view_literals;

static const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");
static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

class WriteCmdShapeRegistrationTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        QueryStatsStoreManager::getRateLimiter(getServiceContext()).configureWindowBased(-1);
        QueryStatsStoreManager::getWriteCmdRateLimiter(getServiceContext())
            .configureSampleBased(1000, 0);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    OpDebug::QueryStatsInfo& queryStatsInfo() {
        return CurOp::get(opCtx())->debug().getQueryStatsInfo();
    }

    write_ops::InsertCommandRequest makeInsert(bool withEncryption = false) {
        auto icr = write_ops::InsertCommandRequest::parseOwned(fromjson(R"({
            insert: "testColl",
            documents: [ { x: 1 } ],
            "$db": "testDB"
        })"sv));
        if (withEncryption) {
            auto wcb = icr.getWriteCommandRequestBase();
            wcb.setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
            icr.setWriteCommandRequestBase(wcb);
        }
        return icr;
    }

    ParsedUpdate makeUpdate(write_ops::UpdateCommandRequest& ucr) {
        auto& op = ucr.getUpdates().front();
        _updateRequest.emplace(op);
        _updateRequest->setNamespaceString(ucr.getNamespace());
        _updateExpCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *_updateRequest).build();
        return uassertStatusOK(parsed_update_command::parse(
            _updateExpCtx, &*_updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    ParsedDelete makeDelete(write_ops::DeleteCommandRequest& dcr) {
        auto& op = dcr.getDeletes().front();
        _deleteRequest.emplace();
        _deleteRequest->setNsString(kTestNss);
        _deleteRequest->setQuery(op.getQ());
        _deleteRequest->setMulti(op.getMulti());
        _deleteExpCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *_deleteRequest).build();
        return uassertStatusOK(parsed_delete_command::parse(
            _deleteExpCtx, &*_deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    enum class UpdateKind { kModifier, kReplacement, kPipeline, kDelta, kTransform };

    write_ops::UpdateCommandRequest makeUpdateCmd(UpdateKind kind = UpdateKind::kModifier,
                                                  bool withEncryption = false) {
        write_ops::UpdateModification mod = [&] {
            switch (kind) {
                case UpdateKind::kModifier:
                    return write_ops::UpdateModification::parseFromClassicUpdate(
                        fromjson("{ $set: { y: 2 } }"));
                case UpdateKind::kReplacement:
                    return write_ops::UpdateModification::parseFromClassicUpdate(
                        fromjson("{ y: 2 }"));
                case UpdateKind::kPipeline:
                    return write_ops::UpdateModification(
                        std::vector<BSONObj>{fromjson("{ $set: { y: 2 } }")});
                case UpdateKind::kDelta: {
                    write_ops::UpdateModification::DiffOptions opts;
                    return write_ops::UpdateModification(fromjson("{ i: { y: 2 } }"),
                                                         write_ops::UpdateModification::DeltaTag{},
                                                         opts);
                }
                case UpdateKind::kTransform:
                    return write_ops::UpdateModification(
                        [](const BSONObj&) -> boost::optional<BSONObj> { return boost::none; });
            }
            MONGO_UNREACHABLE;
        }();

        write_ops::UpdateOpEntry entry(fromjson("{ x: 1 }"), std::move(mod));
        write_ops::UpdateCommandRequest ucr(kTestNss, {entry});
        if (withEncryption) {
            auto wcb = ucr.getWriteCommandRequestBase();
            wcb.setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
            ucr.setWriteCommandRequestBase(wcb);
        }
        return ucr;
    }

    write_ops::DeleteCommandRequest makeDeleteCmd(bool withEncryption = false) {
        auto dcr = write_ops::DeleteCommandRequest::parseOwned(fromjson(R"({
            delete: "testColl",
            deletes: [ { q: { x: 1 }, limit: 0 } ],
            "$db": "testDB"
        })"sv));
        if (withEncryption) {
            auto wcb = dcr.getWriteCommandRequestBase();
            wcb.setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
            dcr.setWriteCommandRequestBase(wcb);
        }
        return dcr;
    }

    // Set by makeUpdate/makeDelete; reuse in computeShapeAndRegisterQueryStats calls so parsed
    // request and registration share the same ExpressionContext.
    boost::intrusive_ptr<ExpressionContext> _updateExpCtx;
    boost::intrusive_ptr<ExpressionContext> _deleteExpCtx;

private:
    ServiceContext::UniqueOperationContext _opCtx;

    // Kept alive for the duration of each test since ParsedUpdate/ParsedDelete hold pointers.
    boost::optional<UpdateRequest> _updateRequest;
    boost::optional<DeleteRequest> _deleteRequest;
};

// Insert: happy path registers a key and stores a hash.
TEST_F(WriteCmdShapeRegistrationTest, InsertRegistersKeyAndHash) {
    auto icr = makeInsert();
    computeInsertShapeAndRegisterQueryStats(opCtx(), icr, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
}

// Insert: encryption information present → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, InsertSkipsRegistrationWhenEncrypted) {
    auto icr = makeInsert(/* withEncryption */ true);
    computeInsertShapeAndRegisterQueryStats(opCtx(), icr, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Insert: feature flag disabled → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, InsertSkipsRegistrationWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard p("featureFlagQueryStatsInsert", false);
    auto icr = makeInsert();
    computeInsertShapeAndRegisterQueryStats(opCtx(), icr, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Update: modifier update (supported) registers a key.
TEST_F(WriteCmdShapeRegistrationTest, UpdateRegistersKeyForModifierUpdate) {
    auto ucr = makeUpdateCmd(UpdateKind::kModifier);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
}

TEST_F(WriteCmdShapeRegistrationTest, UpdateOnSystemCollectionHasNoQueryShapeHash) {
    const auto systemNss = NamespaceString::createNamespaceString_forTest("testDB.system.js");
    auto ucr = makeUpdateCmd();
    ucr.setNamespace(systemNss);
    auto parsedUpdate = makeUpdate(ucr);

    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

// Update: replacement update (supported) registers a key.
TEST_F(WriteCmdShapeRegistrationTest, UpdateRegistersKeyForReplacementUpdate) {
    auto ucr = makeUpdateCmd(UpdateKind::kReplacement);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
}

// Update: pipeline update (supported) registers a key.
TEST_F(WriteCmdShapeRegistrationTest, UpdateRegistersKeyForPipelineUpdate) {
    auto ucr = makeUpdateCmd(UpdateKind::kPipeline);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
}

// Update: delta update (unsupported) → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, UpdateSkipsRegistrationForDeltaUpdate) {
    auto ucr = makeUpdateCmd(UpdateKind::kDelta);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Update: transform update (unsupported) → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, UpdateSkipsRegistrationForTransformUpdate) {
    auto ucr = makeUpdateCmd(UpdateKind::kTransform);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Update: encryption information present → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, UpdateSkipsRegistrationWhenEncrypted) {
    auto ucr = makeUpdateCmd(UpdateKind::kModifier, /* withEncryption */ true);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Update: _id equality query (express path) registers a key but skips the query shape hash (for all
// 3 update types)
TEST_F(WriteCmdShapeRegistrationTest, UpdateModifierExpressPathRegistersKeyButNoShapeHash) {
    write_ops::UpdateOpEntry entry(
        fromjson("{ _id: 1 }"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{ $set: { y: 2 } }")));
    write_ops::UpdateCommandRequest ucr(kTestNss, {entry});
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

TEST_F(WriteCmdShapeRegistrationTest, UpdateReplacementExpressPathRegistersKeyButNoShapeHash) {
    write_ops::UpdateOpEntry entry(
        fromjson("{ _id: 1 }"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{ y: 2 }")));
    write_ops::UpdateCommandRequest ucr(kTestNss, {entry});
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

TEST_F(WriteCmdShapeRegistrationTest, UpdatePipelineExpressPathRegistersKeyButNoShapeHash) {
    write_ops::UpdateOpEntry entry(
        fromjson("{ _id: 1 }"),
        write_ops::UpdateModification(std::vector<BSONObj>{fromjson("{ $set: { y: 2 } }")}));
    write_ops::UpdateCommandRequest ucr(kTestNss, {entry});
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

// Update: _id equality query (express path) with encryption doesn't register a key
TEST_F(WriteCmdShapeRegistrationTest, UpdateExpressPathSkipsRegistrationWhenEncrypted) {
    write_ops::UpdateOpEntry entry(
        fromjson("{ _id: 1 }"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{ $set: { y: 2 } }")));
    write_ops::UpdateCommandRequest ucr(kTestNss, {entry});
    auto wcb = ucr.getWriteCommandRequestBase();
    wcb.setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
    ucr.setWriteCommandRequestBase(wcb);

    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Update: feature flag disabled → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, UpdateSkipsRegistrationWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard p("featureFlagQueryStatsUpdateCommand", false);
    auto ucr = makeUpdateCmd();
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Delete: happy path registers a key.
TEST_F(WriteCmdShapeRegistrationTest, DeleteRegistersKeyAndHash) {
    auto dcr = makeDeleteCmd();
    auto parsedDelete = makeDelete(dcr);
    computeShapeAndRegisterQueryStats<DeleteTypes>(
        opCtx(), _deleteExpCtx, dcr, parsedDelete, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
}

// Delete: encryption information present → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, DeleteSkipsRegistrationWhenEncrypted) {
    auto dcr = makeDeleteCmd(/* withEncryption */ true);
    auto parsedDelete = makeDelete(dcr);
    computeShapeAndRegisterQueryStats<DeleteTypes>(
        opCtx(), _deleteExpCtx, dcr, parsedDelete, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

// Delete: _id equality query (express path) registers a key but skips the query shape hash.
TEST_F(WriteCmdShapeRegistrationTest, DeleteExpressPathRegistersKeyButNoShapeHash) {
    write_ops::DeleteOpEntry entry(fromjson("{ _id: 1 }"), /* multi */ false);
    write_ops::DeleteCommandRequest dcr(kTestNss, {entry});
    auto parsedDelete = makeDelete(dcr);
    computeShapeAndRegisterQueryStats<DeleteTypes>(
        opCtx(), _deleteExpCtx, dcr, parsedDelete, kCollectionType);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

// Delete: feature flag disabled → no key registered.
TEST_F(WriteCmdShapeRegistrationTest, DeleteSkipsRegistrationWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard p("featureFlagQueryStatsDelete", false);
    auto dcr = makeDeleteCmd();
    auto parsedDelete = makeDelete(dcr);
    computeShapeAndRegisterQueryStats<DeleteTypes>(
        opCtx(), _deleteExpCtx, dcr, parsedDelete, kCollectionType);
    ASSERT_EQ(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
}

class BulkWriteRegistrationTest : public WriteCmdShapeRegistrationTest {
public:
    static constexpr auto kMixedOpsCmd = R"({
        bulkWrite: 1,
        ops: [
            { insert: 0, document: { a: 1 } },
            { insert: 0, document: { a: 2 } },
            { update: 0, filter: { x: 1 }, updateMods: { $set: { y: 2 } } },
            { delete: 0, filter: { x: 1 }, multi: false }
        ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        "$db": "admin"
    })"sv;
    static constexpr size_t kInsertIdx = 0;
    static constexpr size_t kUpdateIdx = 2;
    static constexpr size_t kDeleteIdx = 3;

    static BulkWriteCommandRequest parseBulkWrite(std::string_view cmd = kMixedOpsCmd) {
        return BulkWriteCommandRequest::parseOwned(fromjson(cmd));
    }

    ParsedUpdate makeBulkUpdate(const BulkWriteCommandRequest& request, size_t opIdx) {
        const auto& op = get<BulkWriteUpdateOp>(request.getOps()[opIdx]);
        _bulkUpdateRequest.emplace(bulk_write_common::makeUpdateRequestFromUpdateOp(
            opCtx(),
            request.getNsInfo()[op.getNsInfoIdx()],
            &op,
            kUninitializedStmtId,
            request.getLet(),
            request.getBypassEmptyTsReplacement()));
        _bulkUpdateExpCtx =
            ExpressionContextBuilder{}.fromRequest(opCtx(), *_bulkUpdateRequest).build();
        return uassertStatusOK(
            parsed_update_command::parse(_bulkUpdateExpCtx,
                                         &*_bulkUpdateRequest,
                                         makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    ParsedDelete makeBulkDelete(const BulkWriteCommandRequest& request, size_t opIdx) {
        const auto& op = get<BulkWriteDeleteOp>(request.getOps()[opIdx]);
        _bulkDeleteRequest.emplace(
            bulk_write_common::makeDeleteRequestFromDeleteOp(opCtx(),
                                                             request.getNsInfo()[op.getNsInfoIdx()],
                                                             &op,
                                                             kUninitializedStmtId,
                                                             request.getLet()));
        _bulkDeleteExpCtx =
            ExpressionContextBuilder{}.fromRequest(opCtx(), *_bulkDeleteRequest).build();
        return uassertStatusOK(
            parsed_delete_command::parse(_bulkDeleteExpCtx,
                                         &*_bulkDeleteRequest,
                                         makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    void registerUpdate(const BulkWriteCommandRequest& request,
                        size_t opIdx = kUpdateIdx,
                        query_shape::CollectionType collType = kCollectionType) {
        auto parsedUpdate = makeBulkUpdate(request, opIdx);
        registerBulkWriteUpdateOpQueryStats(
            opCtx(), _bulkUpdateExpCtx, request, opIdx, parsedUpdate, collType);
    }

    void registerDelete(const BulkWriteCommandRequest& request,
                        size_t opIdx = kDeleteIdx,
                        query_shape::CollectionType collType = kCollectionType) {
        auto parsedDelete = makeBulkDelete(request, opIdx);
        registerBulkWriteDeleteOpQueryStats(
            opCtx(), _bulkDeleteExpCtx, request, opIdx, parsedDelete, collType);
    }

    void registerInsertGroup(const BulkWriteCommandRequest& request,
                             size_t firstOpIdx = kInsertIdx,
                             query_shape::CollectionType collType = kCollectionType) {
        registerBulkWriteInsertGroupQueryStats(opCtx(), request, firstOpIdx, kTestNss, collType);
    }

    BSONObj registeredKeyBson() {
        ASSERT_NE(queryStatsInfo().key, nullptr);
        return queryStatsInfo().key->toBson(
            opCtx(),
            query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
            SerializationContext::stateDefault());
    }

    query_shape::QueryShapeHash registeredShapeHash() {
        ASSERT_NE(queryStatsInfo().key, nullptr);
        return queryStatsInfo().key->getQueryShapeHash(opCtx(),
                                                       SerializationContext::stateDefault());
    }

    // Takes the registered key out of CurOp, as collecting query stats does, so that the next
    // registration in the same test builds a new key.
    std::unique_ptr<Key> takeKey() {
        queryStatsInfo().keyHash = boost::none;
        return std::move(queryStatsInfo().key);
    }

    void assertRegisteredBulkWriteKey(std::string_view op) {
        ASSERT_TRUE(queryStatsInfo().keyHash.has_value());
        auto keyBson = registeredKeyBson();
        ASSERT_TRUE(keyBson.hasField("errorsOnly")) << keyBson;
        ASSERT_EQ(keyBson["queryShape"]["command"].String(), "bulkWrite") << keyBson;
        ASSERT_EQ(keyBson["queryShape"]["op"].String(), op) << keyBson;
    }

    void assertNothingRegistered() {
        ASSERT_EQ(queryStatsInfo().key, nullptr);
        ASSERT_FALSE(queryStatsInfo().keyHash.has_value());
    }

    // (Generic FCV reference): The feature flag check requires an initialized FCV.
    unittest::EnsureFCV _fcv{multiversion::GenericFCV::kLatest};
    unittest::ServerParameterGuard _flag{"featureFlagQueryStatsAdditionalWriteCommands", true};

private:
    boost::optional<UpdateRequest> _bulkUpdateRequest;
    boost::optional<DeleteRequest> _bulkDeleteRequest;
    boost::intrusive_ptr<ExpressionContext> _bulkUpdateExpCtx;
    boost::intrusive_ptr<ExpressionContext> _bulkDeleteExpCtx;
};

TEST_F(BulkWriteRegistrationTest, UpdateRegistersBulkWriteKey) {
    registerUpdate(parseBulkWrite());
    assertRegisteredBulkWriteKey("update");
}

TEST_F(BulkWriteRegistrationTest, DeleteRegistersBulkWriteKey) {
    registerDelete(parseBulkWrite());
    assertRegisteredBulkWriteKey("delete");
}

TEST_F(BulkWriteRegistrationTest, InsertGroupRegistersBulkWriteKey) {
    registerInsertGroup(parseBulkWrite());
    assertRegisteredBulkWriteKey("insert");
}

// Each insert group registers once, so its key must not depend on which op starts the group or on
// the documents. A group on another namespace gets its own key.
TEST_F(BulkWriteRegistrationTest, InsertGroupKeyDependsOnlyOnNamespace) {
    const auto kOtherNss = NamespaceString::createNamespaceString_forTest("testDB.otherColl");
    auto request = parseBulkWrite(R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { a: 1 } },
               { insert: 0, document: { b: "x", c: [1, 2] } },
               { insert: 1, document: { a: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" }, { ns: "testDB.otherColl" } ],
        "$db": "admin"
    })"sv);
    registerBulkWriteInsertGroupQueryStats(opCtx(), request, 0, kTestNss, kCollectionType);
    auto firstKey = takeKey();
    registerBulkWriteInsertGroupQueryStats(opCtx(), request, 1, kTestNss, kCollectionType);
    ASSERT_EQ(absl::HashOf(*firstKey), absl::HashOf(*takeKey()));
    registerBulkWriteInsertGroupQueryStats(opCtx(), request, 2, kOtherNss, kCollectionType);
    ASSERT_NE(absl::HashOf(*firstKey), absl::HashOf(*takeKey()));
}

TEST_F(BulkWriteRegistrationTest, InsertGroupMustStartAtInsertOpOnItsNamespace) {
    auto request = parseBulkWrite();
    ASSERT_TASSERT_CODE(registerInsertGroup(request, request.getOps().size()), 13830600);
    ASSERT_TASSERT_CODE(registerInsertGroup(request, kUpdateIdx), 13830601);
    ASSERT_TASSERT_CODE(registerBulkWriteInsertGroupQueryStats(
                            opCtx(),
                            request,
                            kInsertIdx,
                            NamespaceString::createNamespaceString_forTest("testDB.otherColl"),
                            kCollectionType),
                        13830602);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, OpShapesUseBulkWriteFieldNames) {
    auto request = parseBulkWrite();
    registerUpdate(request);
    auto updateShape = registeredKeyBson()["queryShape"].Obj().getOwned();
    ASSERT_TRUE(updateShape.hasField("filter")) << updateShape;
    ASSERT_TRUE(updateShape.hasField("updateMods")) << updateShape;
    ASSERT_FALSE(updateShape.hasField("q")) << updateShape;
    ASSERT_FALSE(updateShape.hasField("u")) << updateShape;
    takeKey();
    registerDelete(request);
    auto deleteShape = registeredKeyBson()["queryShape"].Obj().getOwned();
    ASSERT_TRUE(deleteShape.hasField("filter")) << deleteShape;
    ASSERT_FALSE(deleteShape.hasField("q")) << deleteShape;
    takeKey();
    registerInsertGroup(request);
    auto insertShape = registeredKeyBson()["queryShape"].Obj().getOwned();
    ASSERT_TRUE(insertShape.hasField("documents")) << insertShape;
}

TEST_F(BulkWriteRegistrationTest, SkipsWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard flag("featureFlagQueryStatsAdditionalWriteCommands", false);
    auto request = parseBulkWrite();
    registerUpdate(request);
    assertNothingRegistered();
    registerDelete(request);
    assertNothingRegistered();
    registerInsertGroup(request);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, SkipsWhenNsInfoEncrypted) {
    auto request = parseBulkWrite();
    auto nsInfo = request.getNsInfo();
    nsInfo[0].setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
    request.setNsInfo(nsInfo);
    registerUpdate(request);
    assertNothingRegistered();
    registerDelete(request);
    assertNothingRegistered();
    registerInsertGroup(request);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, SkipsForTimeseriesCollType) {
    auto request = parseBulkWrite();
    constexpr auto kTimeseries = query_shape::CollectionType::kTimeseries;
    registerUpdate(request, kUpdateIdx, kTimeseries);
    assertNothingRegistered();
    registerDelete(request, kDeleteIdx, kTimeseries);
    assertNothingRegistered();
    registerInsertGroup(request, kInsertIdx, kTimeseries);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, SkipsWhenSamplerRejects) {
    QueryStatsStoreManager::getWriteCmdRateLimiter(getServiceContext()).configureSampleBased(0, 0);
    auto request = parseBulkWrite();
    registerUpdate(request);
    assertNothingRegistered();
    registerDelete(request);
    assertNothingRegistered();
    registerInsertGroup(request);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, SkipsForDeltaUpdate) {
    auto request = parseBulkWrite();
    auto ops = request.getOps();
    get<BulkWriteUpdateOp>(ops[kUpdateIdx])
        .setUpdateMods(write_ops::UpdateModification(fromjson("{i: {y: 2}}"),
                                                     write_ops::UpdateModification::DeltaTag{},
                                                     write_ops::UpdateModification::DiffOptions{}));
    request.setOps(ops);
    registerUpdate(request);
    assertNothingRegistered();
}

TEST_F(BulkWriteRegistrationTest, OpShapeHashDiffersFromStandalone) {
    auto request = parseBulkWrite();

    registerUpdate(request);
    auto bulkUpdateHash = registeredShapeHash();
    auto bulkUpdateKey = takeKey();
    auto ucr = makeUpdateCmd(UpdateKind::kModifier);
    auto parsedUpdate = makeUpdate(ucr);
    computeShapeAndRegisterQueryStats<UpdateTypes>(
        opCtx(), _updateExpCtx, ucr, parsedUpdate, kCollectionType);
    ASSERT_NE(bulkUpdateHash, registeredShapeHash());
    ASSERT_NE(absl::HashOf(*bulkUpdateKey), absl::HashOf(*takeKey()));

    registerDelete(request);
    auto bulkDeleteHash = registeredShapeHash();
    auto bulkDeleteKey = takeKey();
    write_ops::DeleteOpEntry entry(fromjson("{ x: 1 }"), /* multi */ false);
    write_ops::DeleteCommandRequest dcr(kTestNss, {entry});
    auto parsedDelete = makeDelete(dcr);
    computeShapeAndRegisterQueryStats<DeleteTypes>(
        opCtx(), _deleteExpCtx, dcr, parsedDelete, kCollectionType);
    ASSERT_NE(bulkDeleteHash, registeredShapeHash());
    ASSERT_NE(absl::HashOf(*bulkDeleteKey), absl::HashOf(*takeKey()));

    registerInsertGroup(request);
    auto bulkInsertHash = registeredShapeHash();
    auto bulkInsertKey = takeKey();
    auto icr = makeInsert();
    computeInsertShapeAndRegisterQueryStats(opCtx(), icr, kCollectionType);
    ASSERT_NE(bulkInsertHash, registeredShapeHash());
    ASSERT_NE(absl::HashOf(*bulkInsertKey), absl::HashOf(*takeKey()));
}

TEST_F(BulkWriteRegistrationTest, DoesNotSetOpDebugQueryShapeHash) {
    auto request = parseBulkWrite();
    registerUpdate(request);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
    takeKey();
    registerDelete(request);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
    takeKey();
    registerInsertGroup(request);
    ASSERT_NE(queryStatsInfo().key, nullptr);
    ASSERT_FALSE(CurOp::get(opCtx())->debug().getQueryShapeHash().has_value());
}

// Like standalone, simple _id ops (which skip ParsedFindCommand and may take the express path)
// still register a key.
TEST_F(BulkWriteRegistrationTest, IdFilterOpsRegisterKey) {
    auto request = parseBulkWrite(R"({
        bulkWrite: 1,
        ops: [ { update: 0, filter: { _id: 1 }, updateMods: { $set: { y: 2 } } },
               { delete: 0, filter: { _id: 1 }, multi: false } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        "$db": "admin"
    })"sv);
    ASSERT_FALSE(makeBulkUpdate(request, 0).hasParsedFindCommand());
    registerUpdate(request, 0);
    assertRegisteredBulkWriteKey("update");
    takeKey();
    ASSERT_FALSE(makeBulkDelete(request, 1).hasParsedFindCommand());
    registerDelete(request, 1);
    assertRegisteredBulkWriteKey("delete");
}

}  // namespace
}  // namespace mongo::query_stats

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/write_cmd_shape_registration.h"

#include "mongo/bson/json.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/unittest/server_parameter_guard.h"

namespace mongo::query_stats {
namespace {

using namespace std::literals::string_view_literals;

static const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");
static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

class WriteCmdShapeRegistrationTest : public ServiceContextTest {
public:
    void setUp() override {
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
        _updateRequest->setNamespaceString(kTestNss);
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

}  // namespace
}  // namespace mongo::query_stats

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/logical_agg_stage_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::extension {

namespace {

class HostLogicalAggStageAdapterTest : public unittest::Test {
public:
    void setUp() override {
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

protected:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(HostLogicalAggStageAdapterTest, GetNameFromServerStage) {
    auto sortDs = DocumentSourceSort::create(_expCtx, BSON("a" << 1));

    host_connector::HostLogicalAggStageAdapter adapter(host::LogicalAggStage::make(sortDs.get()));
    LogicalAggStageAPI api(&adapter);

    ASSERT_EQ(api.getName(), DocumentSourceSort::kStageName);
}

TEST_F(HostLogicalAggStageAdapterTest, GetNameFromExtensionStage) {
    auto astNode = new sdk::ExtensionAggStageAstNodeAdapter(
        std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto extensionDs =
        host::DocumentSourceExtensionOptimizable::create(_expCtx, std::move(astHandle));
    host_connector::HostLogicalAggStageAdapter adapter(
        host::LogicalAggStage::make(extensionDs.get()));
    LogicalAggStageAPI api(&adapter);

    ASSERT_EQ(api.getName(), sdk::shared_test_stages::kTransformName);
}

TEST_F(HostLogicalAggStageAdapterTest, GetFilterReturnsEmptyForStageThatHasNoFilter) {
    auto sortDs = DocumentSourceSort::create(_expCtx, BSON("a" << 1));

    host_connector::HostLogicalAggStageAdapter adapter(host::LogicalAggStage::make(sortDs.get()));
    LogicalAggStageAPI api(&adapter);

    ASSERT_BSONOBJ_EQ(api.getFilter(), BSONObj());
}

TEST_F(HostLogicalAggStageAdapterTest, GetFilterReturnsBSONObjForStageThatHasFilter) {
    auto filterBson = BSON("x" << 42);
    auto matchDs = DocumentSourceMatch::create(filterBson, _expCtx);

    host_connector::HostLogicalAggStageAdapter adapter(host::LogicalAggStage::make(matchDs.get()));
    LogicalAggStageAPI api(&adapter);

    ASSERT_BSONOBJ_EQ(api.getFilter(), filterBson);
}

TEST(HostLogicalAggStageAdapterStaticTest, DeletedCopyAndMoveConstructors) {
    static_assert(!std::is_copy_constructible_v<host_connector::HostLogicalAggStageAdapter>,
                  "HostLogicalAggStageAdapter should not be copy constructible");
    static_assert(!std::is_move_constructible_v<host_connector::HostLogicalAggStageAdapter>,
                  "HostLogicalAggStageAdapter should not be move constructible");
    static_assert(!std::is_copy_assignable_v<host_connector::HostLogicalAggStageAdapter>,
                  "HostLogicalAggStageAdapter should not be copy assignable");
    static_assert(!std::is_move_assignable_v<host_connector::HostLogicalAggStageAdapter>,
                  "HostLogicalAggStageAdapter should not be move assignable");
}

}  // namespace

}  // namespace mongo::extension

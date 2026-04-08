/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

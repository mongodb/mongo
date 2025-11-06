/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

class DocumentSourceExtensionOptimizableTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionOptimizableTest() : DocumentSourceExtensionOptimizableTest(_nss) {}
    explicit DocumentSourceExtensionOptimizableTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

protected:
    static inline NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_optimizable_test");

    sdk::ExtensionAggStageDescriptor _noOpAggregationStageDescriptor{
        sdk::shared_test_stages::NoOpAggStageDescriptor::make()};
};

TEST_F(DocumentSourceExtensionOptimizableTest, noOpConstructionSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NoOpAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    ASSERT_EQ(std::string(optimizable->getSourceName()),
              sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
}

TEST_F(DocumentSourceExtensionOptimizableTest, stageCanSerializeForQueryExecution) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NoOpAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    // Test that an extension can provide its own implementation of serialize, that might change
    // the raw spec provided.
    ASSERT_BSONOBJ_EQ(optimizable->serialize(SerializationOptions()).getDocument().toBson(),
                      BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName
                           << "serializedForExecution"));
}

DEATH_TEST_F(DocumentSourceExtensionOptimizableTest, serializeWithWrongOptsFails, "11217800") {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NoOpAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    [[maybe_unused]] auto serialized =
        optimizable->serialize(SerializationOptions::kDebugQueryShapeSerializeOptions);
}

}  // namespace mongo::extension

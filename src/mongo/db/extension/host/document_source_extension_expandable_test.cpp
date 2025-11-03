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

#include "mongo/db/extension/host/document_source_extension_expandable.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/test_stage_id_registrar.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::extension {

class DocumentSourceExtensionExpandableTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionExpandableTest() : DocumentSourceExtensionExpandableTest(_nss) {}
    explicit DocumentSourceExtensionExpandableTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

protected:
    static inline NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_expandable_test");

    sdk::ExtensionAggStageDescriptor _noOpStageDescriptor{
        sdk::shared_test_stages::NoOpAggStageDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _expandToExtAstDescriptor{
        sdk::shared_test_stages::ExpandToExtAstDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _expandToExtParseDescriptor{
        sdk::shared_test_stages::ExpandToExtParseDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _expandToHostParseDescriptor{
        sdk::shared_test_stages::ExpandToHostParseDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _expandToMixedDescriptor{
        sdk::shared_test_stages::ExpandToMixedDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _topDescriptor{sdk::shared_test_stages::TopDescriptor::make()};

    TestStageIdRegistrar _id{sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName,
                             sdk::shared_test_stages::ExpandToExtAstDescriptor::kStageName,
                             sdk::shared_test_stages::ExpandToExtParseDescriptor::kStageName,
                             sdk::shared_test_stages::ExpandToHostParseDescriptor::kStageName,
                             sdk::shared_test_stages::ExpandToMixedDescriptor::kStageName,
                             sdk::shared_test_stages::TopDescriptor::kStageName,
                             std::string(sdk::shared_test_stages::kLeafAName),
                             std::string(sdk::shared_test_stages::kLeafBName),
                             std::string(sdk::shared_test_stages::kLeafCName),
                             std::string(sdk::shared_test_stages::kLeafDName)};
};

TEST_F(DocumentSourceExtensionExpandableTest, ExpandToExtAst) {
    auto rawStage =
        BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName << BSON("foo" << true));

    auto expandable = host::DocumentSourceExtensionExpandable::create(
        sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName,
        getExpCtx(),
        rawStage,
        AggStageDescriptorHandle(&_noOpStageDescriptor));

    auto expanded = expandable->expand();

    ASSERT_EQ(expanded.size(), 1UL);
    auto* optimizable =
        dynamic_cast<host::DocumentSourceExtensionOptimizable*>(expanded.front().get());
    ASSERT(optimizable != nullptr);
    ASSERT_EQ(std::string(optimizable->getSourceName()),
              sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
}

TEST_F(DocumentSourceExtensionExpandableTest, FullParseExpandToHostParse) {
    auto rawStage = BSON(std::string(sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto expandable = host::DocumentSourceExtensionExpandable::create(
        std::string(sdk::shared_test_stages::kExpandToHostParseName),
        getExpCtx(),
        rawStage,
        AggStageDescriptorHandle(&_expandToHostParseDescriptor));

    auto expanded = expandable->expand();
    ASSERT_EQ(expanded.size(), 1UL);
    auto* match = dynamic_cast<DocumentSourceMatch*>(expanded.front().get());
    ASSERT(match != nullptr);
}

TEST_F(DocumentSourceExtensionExpandableTest, FullParseExpandToMixed) {
    auto rawStage = BSON(std::string(sdk::shared_test_stages::kExpandToMixedName) << BSONObj());
    auto expandable = host::DocumentSourceExtensionExpandable::create(
        std::string(sdk::shared_test_stages::kExpandToMixedName),
        getExpCtx(),
        rawStage,
        AggStageDescriptorHandle(&_expandToMixedDescriptor));

    auto expanded = expandable->expand();
    ASSERT_EQ(expanded.size(), 3UL);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);

    auto* first = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it0->get());
    ASSERT(first != nullptr);
    ASSERT_EQ(std::string(first->getSourceName()), std::string(sdk::shared_test_stages::kNoOpName));

    auto* second = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it1->get());
    ASSERT(second != nullptr);
    ASSERT_EQ(std::string(second->getSourceName()),
              std::string(sdk::shared_test_stages::kNoOpName));

    auto* third = dynamic_cast<DocumentSourceMatch*>(it2->get());
    ASSERT(third != nullptr);
}

TEST_F(DocumentSourceExtensionExpandableTest, FullParseExpandRecursesMultipleLevels) {
    auto rawStage = BSON(std::string(sdk::shared_test_stages::kTopName) << BSONObj());
    auto expandable = host::DocumentSourceExtensionExpandable::create(
        std::string(sdk::shared_test_stages::kTopName),
        getExpCtx(),
        rawStage,
        AggStageDescriptorHandle(&_topDescriptor));

    auto expanded = expandable->expand();
    ASSERT_EQ(expanded.size(), 4UL);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);
    const auto it3 = std::next(expanded.begin(), 3);

    auto* first = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it0->get());
    ASSERT(first != nullptr);
    ASSERT_EQ(std::string(first->getSourceName()),
              std::string(sdk::shared_test_stages::kLeafAName));

    auto* second = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it1->get());
    ASSERT(second != nullptr);
    ASSERT_EQ(std::string(second->getSourceName()),
              std::string(sdk::shared_test_stages::kLeafBName));

    auto* third = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it2->get());
    ASSERT(third != nullptr);
    ASSERT_EQ(std::string(third->getSourceName()),
              std::string(sdk::shared_test_stages::kLeafCName));

    auto* fourth = dynamic_cast<host::DocumentSourceExtensionOptimizable*>(it3->get());
    ASSERT(fourth != nullptr);
    ASSERT_EQ(std::string(fourth->getSourceName()),
              std::string(sdk::shared_test_stages::kLeafDName));
}

}  // namespace mongo::extension

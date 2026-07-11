// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class MongosProcessInterfaceForTest : public MongosProcessInterface {
public:
    using MongosProcessInterface::MongosProcessInterface;

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fieldPaths) const override {
        return hasSupportingIndexForFields;
    }

    SupportingUniqueIndex hasSupportingIndexForFields{SupportingUniqueIndex::Full};
};

class MongosProcessInterfaceTest : public AggregationContextFixture {
public:
    MongosProcessInterfaceTest() {
        getExpCtx()->setInRouter(true);
        _processInterface = std::make_shared<MongosProcessInterfaceForTest>(nullptr);
        // Set the process interface on expCtx so isExpectedToExecuteQueries() returns true.
        getExpCtx()->setMongoProcessInterface(_processInterface);
    }

    auto getProcessInterface() {
        return _processInterface;
    }

private:
    std::shared_ptr<MongosProcessInterfaceForTest> _processInterface;
};

TEST_F(MongosProcessInterfaceTest,
       FailsToEnsureFieldsUniqueIftargetCollectionPlacementVersionIsSpecified) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = getProcessInterface();

    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51179);
}

TEST_F(MongosProcessInterfaceTest, FailsToEnsureFieldsUniqueIfNotSupportedByIndex) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion = boost::none;
    auto processInterface = getProcessInterface();

    processInterface->hasSupportingIndexForFields =
        MongoProcessInterface::SupportingUniqueIndex::None;
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"x"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51190);
}
}  // namespace
}  // namespace mongo

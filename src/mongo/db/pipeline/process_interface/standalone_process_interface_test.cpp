// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class MongoProcessInterfaceForTest : public StandaloneProcessInterface {
public:
    using StandaloneProcessInterface::StandaloneProcessInterface;

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fields) const override {
        return hasSupportingIndexForFields;
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        // Assume it always matches for our tests here.
        return;
    }

    SupportingUniqueIndex hasSupportingIndexForFields{SupportingUniqueIndex::Full};
};

class ProcessInterfaceStandaloneTest : public ShardServerTestFixture {
protected:
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB1");
    const std::string_view coll = "testColl";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, coll);

    OperationContext* opCtx() {
        return operationContext();
    }

    boost::intrusive_ptr<ExpressionContextForTest> getExpCtx() {
        return _expCtx;
    }

    void installUntrackedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss) {
        const auto untrackedCollectionMetadata = CollectionMetadata::UNTRACKED();
        CollectionShardingRuntime::acquireExclusive(opCtx, nss)
            ->setCollectionMetadata(opCtx, untrackedCollectionMetadata);
    }

    void setUp() override {
        ShardServerTestFixture::setUp();
        _expCtx = make_intrusive<ExpressionContextForTest>(opCtx(), nss);
    }

    void createTimeseriesCollection() {
        createTestCollection(
            opCtx(),
            nss,
            BSON("create" << coll << "timeseries" << BSON("timeField" << "timestamp")));

        NamespaceString underlyingNss = nss;
        auto viewDefinition = CollectionCatalog::get(opCtx())->lookupView(opCtx(), nss);
        if (viewDefinition) {
            underlyingNss = viewDefinition->viewOn();
        }

        installUntrackedCollectionMetadata(opCtx(), underlyingNss);
    }

    auto makeProcessInterface() {
        return std::make_unique<MongoProcessInterfaceForTest>(nullptr);
    }

private:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(ProcessInterfaceStandaloneTest,
       FailsToEnsureFieldsUniqueIftargetCollectionPlacementVersionIsSpecifiedOnMongos) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();

    // Test that 'targetCollectionPlacementVersion' is not accepted if not from router.
    expCtx->setFromRouter(false);
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51123);

    // Test that 'targetCollectionPlacementVersion' is accepted if from router.
    expCtx->setFromRouter(true);
    auto [joinKey, chunkVersion, supportingUniqueIndex] =
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString());
    ASSERT_EQ(joinKey.size(), 1UL);
    ASSERT_EQ(joinKey.count(FieldPath("_id")), 1UL);
    ASSERT(chunkVersion);
    ASSERT_EQ(*chunkVersion, *targetCollectionPlacementVersion);
    ASSERT_EQ(supportingUniqueIndex, MongoProcessInterface::SupportingUniqueIndex::Full);
}

TEST_F(ProcessInterfaceStandaloneTest, FailsToEnsureFieldsUniqueIfOnTimeseriesCollection) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();
    expCtx->setFromRouter(true);
    createTimeseriesCollection();

    ASSERT_THROWS_CODE(processInterface->ensureFieldsUniqueOrResolveDocumentKey(
                           expCtx, {{"_id"}}, targetCollectionPlacementVersion, nss),
                       AssertionException,
                       1074330);
}

TEST_F(ProcessInterfaceStandaloneTest, FailsToEnsureFieldsUniqueIfJoinFieldsAreNotSentFromMongos) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();

    expCtx->setFromRouter(true);
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, boost::none, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51124);
}

TEST_F(ProcessInterfaceStandaloneTest,
       FailsToEnsureFieldsUniqueIfFieldsDoesNotHaveSupportingUniqueIndex) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion = boost::none;
    auto processInterface = makeProcessInterface();

    expCtx->setFromRouter(false);
    processInterface->hasSupportingIndexForFields =
        MongoProcessInterface::SupportingUniqueIndex::None;
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"x"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51183);
}
}  // namespace
}  // namespace mongo

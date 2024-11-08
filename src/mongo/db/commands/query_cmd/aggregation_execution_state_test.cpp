/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/commands/query_cmd/aggregation_execution_state.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * Test the basic functionality of each subclass of AggCatalogState.
 */
class AggregationExecutionStateTest : public CatalogTestFixture {
protected:
    /**
     * Add a collection with the given name to the catalog.
     */
    void createCollection(StringData coll) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);
        auto opCtx = operationContext();
        DBDirectClient client{opCtx};
        auto cmd = BSON("create" << coll);
        BSONObj result;
        ASSERT_TRUE(client.runCommand(nss.dbName(), cmd, result));
    }

    /**
     * Add a view with the given name to the catalog.
     */
    void createView(StringData viewName, StringData collName) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", collName);
        auto opCtx = operationContext();
        DBDirectClient client{opCtx};
        auto cmd = BSON("create" << viewName << "viewOn" << collName << "pipeline"
                                 << BSON_ARRAY(BSON("$match" << BSON("a" << 1))));
        BSONObj result;
        ASSERT_TRUE(client.runCommand(nss.dbName(), cmd, result));
    }

    /**
     * Create an AggExState instance that one might see for a typical query.
     */
    std::unique_ptr<AggExState> createDefaultAggExState(StringData coll) {
        auto opCtx = operationContext();

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);

        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSONObj{} << "cursor" << BSONObj{});
        std::vector<BSONObj> pipeline;
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        return std::make_unique<AggExState>(
            opCtx, *_request, *_lpp, _cmdObj, _privileges, _externalSources);
    }

    /**
     * Create an AggExState instance that one might see for change stream query.
     */
    std::unique_ptr<AggExState> createOplogAggExState(StringData coll) {
        auto opCtx = operationContext();

        // We will wait indefinitely in this unit test for the read concern to be set unless we set
        // it explicitly here.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs{repl::ReadConcernLevel::kLocalReadConcern};

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);
        BSONObj changeStreamStage = BSON("$changeStream" << BSONObj{});

        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSON_ARRAY(changeStreamStage)
                                   << "cursor" << BSONObj{});
        std::vector<BSONObj> pipeline{changeStreamStage};
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        auto aggExState = std::make_unique<AggExState>(
            opCtx, *_request, *_lpp, _cmdObj, _privileges, _externalSources);

        return aggExState;
    }

    /**
     * Create an AggExState instance that one might see for a query that is not on any particular
     * collection.
     */
    std::unique_ptr<AggExState> createCollectionlessAggExState() {
        auto opCtx = operationContext();

        StringData coll = "$cmd.aggregate"_sd;
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);

        BSONObj documentsStage = BSON("$documents" << BSON_ARRAY(BSON("a" << 1)));
        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSON_ARRAY(documentsStage) << "cursor"
                                   << BSONObj{});
        std::vector<BSONObj> pipeline{documentsStage};
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        return std::make_unique<AggExState>(
            opCtx, *_request, *_lpp, _cmdObj, _privileges, _externalSources);
    }

private:
    std::unique_ptr<AggregateCommandRequest> _request;
    std::unique_ptr<LiteParsedPipeline> _lpp;
    PrivilegeVector _privileges;
    BSONObj _cmdObj;
    std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>> _externalSources;
};

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogState) {
    StringData coll{"coll"};
    createCollection(coll);
    std::unique_ptr<AggExState> aggExState = createDefaultAggExState(coll);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    // This call should not throw.
    aggCatalogState->validate();

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    ASSERT_EQ(aggCatalogState->getCatalog(), CollectionCatalog::latest(operationContext()));

    ASSERT_TRUE(aggCatalogState->getUUID().has_value());

    // This call should not throw.
    aggCatalogState->relinquishLocks();
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogStateView) {
    StringData coll{"coll"};
    StringData view{"view"};
    createCollection(coll);
    createView(view, coll);
    std::unique_ptr<AggExState> aggExState = createDefaultAggExState(view);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    // This call should not throw.
    aggCatalogState->validate();

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    // It's a view so apparently there is no main collection, per se.
    ASSERT_FALSE(aggCatalogState->getCollections().hasMainCollection());

    ASSERT_EQ(aggCatalogState->getCatalog(), CollectionCatalog::latest(operationContext()));

    ASSERT_FALSE(aggCatalogState->getUUID().has_value());

    // This call should not throw.
    aggCatalogState->relinquishLocks();
}

TEST_F(AggregationExecutionStateTest, CreateOplogAggCatalogState) {
    StringData coll{"coll"};
    createCollection(coll);
    std::unique_ptr<AggExState> aggExState = createOplogAggExState(coll);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    // This call should not throw.
    aggCatalogState->validate();

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    ASSERT_EQ(aggCatalogState->getCatalog(), CollectionCatalog::latest(operationContext()));

    // UUIDs are not used for change stream queries.
    ASSERT_FALSE(aggCatalogState->getUUID().has_value());

    // This call should not throw.
    aggCatalogState->relinquishLocks();
}

TEST_F(AggregationExecutionStateTest, CreateOplogAggCatalogStateFailsOnView) {
    StringData coll{"coll"};
    StringData view{"view"};
    createCollection(coll);
    createView(view, coll);

    std::unique_ptr<AggExState> aggExState = createOplogAggExState(view);

    // This will call the validate() method which will fail because you cannot open a change stream
    // on a view.
    ASSERT_THROWS_CODE(
        aggExState->createAggCatalogState(), DBException, ErrorCodes::CommandNotSupportedOnView);
}

}  // namespace
}  // namespace mongo

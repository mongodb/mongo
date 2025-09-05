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

/**
 * This file contains tests for mongo/db/exec/text_or.cpp
 */

#include "mongo/db/exec/classic/text_or.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/classic/working_set_common.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

namespace {

static const NamespaceString kNss =
    NamespaceString::createNamespaceString_forTest("db.text_or_test");
static const StringData kIndexName = "x_test";
static const std::vector<BSONObj> kMovieDocs = {
    BSON(
        "_id"
        << 1 << "movie"
        << "The Devil Wears Prada"
        << "quote"
        << "We Have All The Published Harry Potter Books. The Twins Want To Know What Happens Next."
        << "rating" << 8.7),
    BSON("_id" << 2 << "movie"
               << "Arrival"
               << "quote"
               << "But now I'm not so sure I believe in beginnings and endings. There are days "
                  "that define your story beyond your life. Like the day they arrived."
               << "rating" << 9.2),
    BSON("_id" << 3 << "movie"
               << "Whiplash"
               << "quote"
               << "There are no two words in the English language more harmful than good job"
               << "rating" << 9.5),
    BSON("_id" << 4 << "movie"
               << "La La Land"
               << "quote"
               << "It's conflict and it's compromise, and it's just... it's brand new every time. "
                  "It's brand new every night."
               << "rating" << 9.0),
    BSON("_id" << 5 << "movie"
               << "Formula 1: The Movie"
               << "quote"
               << "Hope is not a strategy. Create your own breaks."
               << "rating" << 8.8)};

class TextOrStageTest : public ServiceContextMongoDTest {
public:
    TextOrStageTest()
        : ServiceContextMongoDTest(Options{}.useMockClock(true)),
          _opCtx(makeOperationContext()),
          _expCtx(ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kNss).build()) {
        _setUpCollectionAndIdx(kIndexName /*indexName*/,
                               BSON("quote" << "text") /*indexKeys*/,
                               kMovieDocs /*docsToInsert*/);
    }

    TextOrStage makeTextOr(size_t keyPrefixSize = 1, const MatchExpression* filter = nullptr) {
        return TextOrStage(_expCtx.get(), keyPrefixSize, &_ws, filter, acquireCollForRead(kNss));
    }

    /**
     * Executes a TextOrStage until completion and collects all results. TextOr returns a
     * vector of pairs containing RecordId and score for each result.
     */
    std::vector<std::pair<RecordId, double>> executeTextOrAndCollectResults(
        TextOrStage& textOr, const std::vector<PlanStage::StageState>& expectedStageStates) {

        WorkingSetID id = WorkingSet::INVALID_ID;
        std::vector<std::pair<RecordId, double>> results;

        for (const auto& expectedState : expectedStageStates) {
            PlanStage::StageState state = textOr.work(&id);
            ASSERT_EQ(state, expectedState);

            if (state == PlanStage::ADVANCED) {
                WorkingSetMember* member = _ws.get(id);
                double score = member->metadata().getTextScore();

                results.emplace_back(member->recordId, score);
            }
        }

        return results;
    }

    /**
     * Simulates a text index scan results by creating a MockStage that returns a single document
     * match. We create a MockStage with a working set member containing the document's matched
     * recordId and index key data.
     */
    void createTextOrChild(TextOrStage& textOr,
                           RecordId recordId,
                           StringData term,
                           double score,
                           StringData indexName = kIndexName) {
        auto childStage = std::make_unique<MockStage>(_expCtx.get(), &_ws);

        // Keep raw pointer before transferring ownership to the TextOrStage.
        MockStage& stagePtr = *childStage;
        textOr.addChild(std::move(childStage));

        // Register the index.
        const IndexDescriptor* indexDescriptor = getIndexDescriptor(indexName);
        WorkingSetRegisteredIndexId indexId =
            _ws.registerIndexIdent(indexDescriptor->getEntry()->getIdent());

        WorkingSetID wsid = _ws.allocate();
        WorkingSetMember* member = _ws.get(wsid);
        member->recordId = recordId;
        member->keyData.push_back(
            IndexKeyDatum(indexDescriptor->keyPattern(),
                          BSON("" << 1 << "" << term << "" << score << ""
                                  << "english"),
                          indexId,
                          shard_role_details::getRecoveryUnit(_opCtx.get())->getSnapshotId()));

        // TextOr expects that its working set members pass it index data.
        _ws.transitionToRecordIdAndIdx(wsid);

        // Add to the mock stage.
        stagePtr.enqueueAdvanced(wsid);
    }

    const IndexDescriptor* getIndexDescriptor(StringData name) {
        return acquireCollForRead(kNss).getCollectionPtr()->getIndexCatalog()->findIndexByName(
            _opCtx.get(), name);
    }

    CollectionAcquisition acquireCollForRead(const NamespaceString& nss) {
        return acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    WorkingSet* ws() {
        return &_ws;
    }

    ExpressionContext* expCtx() {
        return _expCtx.get();
    }

private:
    /*
     * TextOr requires a collection and an index to do work on.
     */
    void _setUpCollectionAndIdx(StringData indexName,
                                const BSONObj& indexKeys,
                                const std::vector<BSONObj>& docsToInsert) {
        // We need to be a primary to create a new collection.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        ASSERT(replCoord);
        auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
        ASSERT(replCoordMock);
        ASSERT_OK(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));

        CreateCommand cmd = CreateCommand(kNss);
        uassertStatusOK(createCollection(_opCtx.get(), cmd));

        // Create index.
        DBDirectClient client(_opCtx.get());
        IndexSpec spec;
        spec.name(indexName);
        spec.addKeys(indexKeys);
        client.createIndex(kNss, spec);

        // Insert documents in the collection so TextOr can return scores.
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments(docsToInsert);
        auto insertReply = client.insert(insertOp);

        ASSERT_EQ(insertReply.getN(), docsToInsert.size());
        ASSERT_FALSE(insertReply.getWriteErrors());
        return;
    }

    WorkingSet _ws;

    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(TextOrStageTest, EmptyChildren) {
    auto textOr = makeTextOr();

    std::vector<PlanStage::StageState> expectedStates = {
        PlanStage::NEED_TIME,  // TextOr's first work() will always initialize the record cursor.
        PlanStage::IS_EOF,
    };
    executeTextOrAndCollectResults(textOr, expectedStates);

    ASSERT_EQ(textOr.getMemoryTracker_forTest().inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(textOr.getMemoryTracker_forTest().peakTrackedMemoryBytes(), 0);
    auto stats = static_cast<const TextOrStats*>(textOr.getSpecificStats());
    ASSERT_EQUALS(stats->peakTrackedMemBytes, 0);
}

TEST_F(TextOrStageTest, OneChild) {
    auto textOr = makeTextOr();
    createTextOrChild(textOr, RecordId(3), "harmful", 9.5);
    ASSERT_EQ(textOr.getChildren().size(), 1);

    std::vector<PlanStage::StageState> expectedStates = {PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::ADVANCED,
                                                         PlanStage::IS_EOF};

    auto results = executeTextOrAndCollectResults(textOr, expectedStates);

    ASSERT_EQ(results.size(), 1);
    ASSERT_EQ(results[0].first, RecordId(3));
    ASSERT_EQ(results[0].second, 9.5);

    auto* stats = static_cast<const TextOrStats*>(textOr.getSpecificStats());
    ASSERT_EQUALS(1, stats->fetches);
    ASSERT_GT(stats->peakTrackedMemBytes, 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().peakTrackedMemoryBytes(), 0);
}

TEST_F(TextOrStageTest, ChildrenNoResults) {
    auto textOr = makeTextOr();

    // Create a mock stage with no results.
    auto emptyChildStage = std::make_unique<MockStage>(expCtx(), ws());
    textOr.addChild(std::move(emptyChildStage));

    ASSERT_EQ(textOr.getChildren().size(), 1);

    std::vector<PlanStage::StageState> expectedStates = {
        PlanStage::NEED_TIME,
        PlanStage::NEED_TIME,
        PlanStage::IS_EOF,
    };
    auto results = executeTextOrAndCollectResults(textOr, expectedStates);

    // No results should be returned and no memory should have been tracked.
    ASSERT_EQ(results.size(), 0);

    auto stats = static_cast<const TextOrStats*>(textOr.getSpecificStats());
    ASSERT_EQ(stats->peakTrackedMemBytes, 0);
    ASSERT_EQ(textOr.getMemoryTracker_forTest().inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(textOr.getMemoryTracker_forTest().peakTrackedMemoryBytes(), 0);
}

TEST_F(TextOrStageTest, MultipleChildren) {
    auto textOr = makeTextOr();
    createTextOrChild(textOr, RecordId(1), "published", 8.7);
    createTextOrChild(textOr, RecordId(5), "strategy", 8.8);

    ASSERT_EQ(textOr.getChildren().size(), 2);

    std::vector<PlanStage::StageState> expectedStates = {PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::NEED_TIME,
                                                         PlanStage::ADVANCED,
                                                         PlanStage::ADVANCED,
                                                         PlanStage::IS_EOF};
    auto results = executeTextOrAndCollectResults(textOr, expectedStates);

    // Sort the results, as the result order is not guaranteed.
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0].first, RecordId(1));  // Devil Wears Prada.
    ASSERT_EQ(results[0].second, 8.7);
    ASSERT_EQ(results[1].first, RecordId(5));  // Formula 1.
    ASSERT_EQ(results[1].second, 8.8);

    auto stats = static_cast<const TextOrStats*>(textOr.getSpecificStats());
    ASSERT_EQ(stats->fetches, 2);
    ASSERT_GT(stats->peakTrackedMemBytes, 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().peakTrackedMemoryBytes(), 0);
}

TEST_F(TextOrStageTest, SameDocumentMultipleTerms) {
    auto textOr = makeTextOr();
    createTextOrChild(textOr, RecordId(4), "conflict", 9.0);
    createTextOrChild(textOr, RecordId(4), "compromise", 9.0);

    ASSERT_EQ(textOr.getChildren().size(), 2);

    std::vector<PlanStage::StageState> expectedStates = {
        PlanStage::NEED_TIME,
        PlanStage::NEED_TIME,
        PlanStage::NEED_TIME,
        PlanStage::NEED_TIME,
        PlanStage::NEED_TIME,
        PlanStage::ADVANCED,
    };
    auto results = executeTextOrAndCollectResults(textOr, expectedStates);

    // Verify we get one document with combined score (9.0 + 9.0).
    ASSERT_EQ(results.size(), 1);
    ASSERT_EQ(results[0].first, RecordId(4));  // La La Land
    ASSERT_EQ(results[0].second, 18.0);

    auto stats = static_cast<const TextOrStats*>(textOr.getSpecificStats());
    ASSERT_EQ(stats->fetches, 1);  // Only one fetch occurs since it's the same document.
    ASSERT_GT(stats->peakTrackedMemBytes, 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(textOr.getMemoryTracker_forTest().peakTrackedMemoryBytes(), 0);
}
}  // namespace
}  // namespace mongo

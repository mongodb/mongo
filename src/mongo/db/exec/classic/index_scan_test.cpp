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

#include "mongo/db/exec/classic/index_scan.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
namespace mongo {
namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class IndexScanTest : public ServiceContextMongoDTest {
public:
    IndexScanTest()
        : _opCtx{makeOperationContext()},
          _expCtx{ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kNss).build()},
          _ws{},
          _client{std::make_unique<DBDirectClient>(_opCtx.get())} {
        // Make sure we are the primary so we can create collections and indexes.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        ASSERT(replCoord);
        auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
        ASSERT(replCoordMock);
        ASSERT_OK(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

    void createIndex(StringData name, BSONObj keys) {
        _client->createCollection(kNss);
        IndexSpec spec;
        spec.name(name);
        spec.addKeys(keys);
        _client->createIndex(kNss, spec);
    }

    CollectionAcquisition getCollection() {
        return acquireCollection(_opCtx.get(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     _opCtx.get(), kNss, AcquisitionPrerequisites::kRead),
                                 MODE_IS);
    }


    const IndexDescriptor* getIndexDescriptor(StringData name) {
        return getCollection().getCollectionPtr()->getIndexCatalog()->findIndexByName(_opCtx.get(),
                                                                                      name);
    }

    void insertDocs(std::vector<BSONObj> docs) {
        write_ops::InsertCommandRequest insertOp(kNss);
        const int numDocs = docs.size();
        insertOp.setDocuments(std::move(docs));
        auto insertReply = _client->insert(insertOp);
        ASSERT_EQ(insertReply.getN(), numDocs);
        ASSERT_FALSE(insertReply.getWriteErrors());
    }

    std::vector<BSONObj> executeIndexScanStage(IndexScan& indexScan, bool expectMemoryUsage) {
        std::vector<BSONObj> results;
        WorkingSetID wsid = WorkingSet::INVALID_ID;

        uint64_t peakTrackedMemoryBytes = 0;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            state = indexScan.work(&wsid);

            if (state == PlanStage::ADVANCED) {
                const auto& tracker = indexScan.getMemoryTracker_forTest();
                uint64_t inUseTrackedMemoryBytes = tracker.inUseTrackedMemoryBytes();
                if (expectMemoryUsage) {
                    peakTrackedMemoryBytes =
                        std::max(inUseTrackedMemoryBytes, peakTrackedMemoryBytes);
                    // If we are deduping and we have processed a record, there should be non-zero
                    // memory usage.
                    ASSERT_GT(inUseTrackedMemoryBytes, 0);
                    ASSERT_GTE(static_cast<uint64_t>(tracker.peakTrackedMemoryBytes()),
                               peakTrackedMemoryBytes);
                } else {
                    ASSERT_EQ(0, inUseTrackedMemoryBytes);
                }

                WorkingSetMember* member = _ws.get(wsid);
                ASSERT_EQ(member->keyData.size(), 1);
                results.push_back(member->keyData[0].keyData);
            }
        }

        return results;
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    WorkingSet _ws;
    std::unique_ptr<DBDirectClient> _client;
};

TEST_F(IndexScanTest, IndexScanStageReturnsEOF) {
    const auto indexName = "IndexScanStageReturnsEOF";
    const auto indexKeys = BSON("a" << 1);
    const MatchExpression* matchExpr = nullptr;

    createIndex(indexName, indexKeys);

    const auto coll = getCollection();
    const auto* idxDesc = getIndexDescriptor(indexName);
    IndexScanParams params{_opCtx.get(), coll.getCollectionPtr(), idxDesc};

    IndexScan indexScan(_expCtx.get(), coll, std::move(params), &_ws, matchExpr);
    WorkingSetID wsid = WorkingSet::INVALID_ID;
    ASSERT_EQUALS(indexScan.work(&wsid), PlanStage::IS_EOF);
}

TEST_F(IndexScanTest, BasicIndex) {
    const auto indexName = "BasicIndex";
    const auto indexKeys = BSON("a" << 1);
    const MatchExpression* matchExpr = nullptr;
    createIndex(indexName, indexKeys);

    insertDocs({BSON("_id" << 1 << "a" << 10),
                BSON("_id" << 2 << "a" << 5),
                BSON("_id" << 3 << "a" << 20)});

    const auto coll = getCollection();
    const auto* idxDesc = getIndexDescriptor(indexName);
    IndexScanParams params{_opCtx.get(), coll.getCollectionPtr(), idxDesc};

    IndexScan indexScan(_expCtx.get(), coll, std::move(params), &_ws, matchExpr);

    std::vector<BSONObj> results = executeIndexScanStage(indexScan, false /*expectMemoryUsage*/);

    std::vector<BSONObj> expectedKeyInfo = {BSON("" << 5), BSON("" << 10), BSON("" << 20)};
    ASSERT_EQ(results.size(), expectedKeyInfo.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_BSONOBJ_EQ(results[i], expectedKeyInfo[i]);
    }
}

TEST_F(IndexScanTest, BasicMultiKeyIndex) {
    const auto indexName = "BasicMultiKeyIndex";
    const auto indexKeys = BSON("a" << 1);
    const MatchExpression* matchExpr = nullptr;
    createIndex(indexName, indexKeys);

    // Create test documents with an array for 'a'.
    insertDocs({BSON("_id" << 1 << "a" << BSONArray(BSON("0" << 1 << "1" << 10))),
                BSON("_id" << 2 << "a" << BSONArray(BSON("0" << 3 << "1" << 4))),
                BSON("_id" << 3 << "a" << BSONArray(BSON("0" << 5 << "1" << 6)))});


    const auto coll = getCollection();
    const auto* idxDesc = getIndexDescriptor(indexName);

    // Make sure index is a multikey index as 'a' is an array.
    ASSERT_EQ(idxDesc->getEntry()->isMultikey(_opCtx.get(), coll.getCollectionPtr()), true);

    IndexScanParams params{_opCtx.get(), coll.getCollectionPtr(), idxDesc};
    //'shouldDedup' should be true for multiKey indexes as there may be multiple keys per recordId.
    params.shouldDedup = true;
    IndexScan indexScan(_expCtx.get(), coll, std::move(params), &_ws, matchExpr);

    std::vector<BSONObj> results = executeIndexScanStage(indexScan, true /*expectMemoryUsage*/);

    // IndexScan should return a single key from each document as 'shouldDedup' is true.
    std::vector<BSONObj> expectedKeyInfo = {BSON("" << 1), BSON("" << 3), BSON("" << 5)};
    ASSERT_EQ(results.size(), expectedKeyInfo.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_BSONOBJ_EQ(results[i], expectedKeyInfo[i]);
    }
}


}  // namespace
}  // namespace mongo

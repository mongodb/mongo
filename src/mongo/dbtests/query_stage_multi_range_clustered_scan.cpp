// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Tests for db/exec/classic/multi_range_clustered_scan.cpp.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/multi_range_clustered_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace multi_range_clustered_scan_test {

class MultiRangeClusteredScanTest : public unittest::Test {
public:
    MultiRangeClusteredScanTest() : _client(&_opCtx) {}

    class ScopedCollectionDeleter {
    public:
        ScopedCollectionDeleter(OperationContext* opCtx, NamespaceString nss)
            : _opCtx(opCtx), _nss(nss) {}
        ~ScopedCollectionDeleter() {
            AutoGetDb autoDb(_opCtx, _nss.dbName(), MODE_IX);
            if (!autoDb.getDb())
                return;

            AutoGetCollection autoColl(_opCtx, _nss, MODE_X);
            if (!*autoColl)
                return;

            WriteUnitOfWork wuow(_opCtx);
            ASSERT_OK(autoDb.getDb()->dropCollection(_opCtx, _nss));
            wuow.commit();
        }
        ScopedCollectionDeleter(const ScopedCollectionDeleter&& other) = delete;

    private:
        OperationContext* _opCtx;
        NamespaceString _nss;
    };

    ScopedCollectionDeleter createClusteredCollection(const NamespaceString& ns) {
        AutoGetCollection autoColl(&_opCtx, ns, MODE_IX);
        auto db = autoColl.ensureDbExists(&_opCtx);
        WriteUnitOfWork wuow(&_opCtx);
        CollectionOptions collOptions;
        collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        const bool createIdIndex = false;
        db->createCollection(&_opCtx, ns, collOptions, createIdIndex);
        wuow.commit();
        return {&_opCtx, ns};
    }

    void insertDocument(const NamespaceString& ns, const BSONObj& doc) {
        _client.insert(ns, doc);
    }

    // Build a RecordIdRange from integer _id bounds. boost::none means unbounded on that side.
    static RecordIdRange makeIntRange(boost::optional<int> minId,
                                      bool minInclusive,
                                      boost::optional<int> maxId,
                                      bool maxInclusive) {
        RecordIdRange range;
        boost::optional<RecordIdBound> minBound;
        boost::optional<RecordIdBound> maxBound;
        if (minId)
            minBound =
                RecordIdBound(record_id_helpers::keyForElem(BSON("_id" << *minId).firstElement()));
        if (maxId)
            maxBound =
                RecordIdBound(record_id_helpers::keyForElem(BSON("_id" << *maxId).firstElement()));
        range.intersectRange(minBound, maxBound, minInclusive, maxInclusive);
        return range;
    }

    /**
     * Runs the stage with the given rangeList in the given direction over a clustered collection
     * populated with 'docs' (whose _id values are integers), and asserts that the returned
     * documents and the stats (docsTested, seeks) match what the naive iteration model would
     * produce.
     */
    void checkClusteredScanResult(const NamespaceString& ns,
                                  CollectionScanParams::Direction direction,
                                  const RecordIdRangeList& rangeList,
                                  const std::vector<BSONObj>& docs,
                                  const MatchExpression* filter = nullptr) {
        const auto coll = acquireCollectionMaybeLockFree(
            &_opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                &_opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
        ASSERT(coll.getCollectionPtr()->isClustered());

        MultiRangeClusteredScanParams params;
        params.direction = direction;
        params.rangeList = rangeList;

        WorkingSet ws;
        auto scan =
            std::make_unique<MultiRangeClusteredScan>(_expCtx.get(), coll, params, &ws, filter);

        std::vector<BSONObj> actualDocs;
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->getState() == WorkingSetMember::RID_AND_OBJ);
                actualDocs.push_back(member->doc.value().toBson().getOwned());
            }
        }

        auto* stats = dynamic_cast<const CollectionScanStats*>(scan->getSpecificStats());

        // Compute expected results and stats naively by iterating the docs in sorted order.
        std::vector<BSONObj> sortedDocs = docs;
        std::sort(sortedDocs.begin(), sortedDocs.end(), [&](const BSONObj& a, const BSONObj& b) {
            // FORWARD : sort in increasing _id order. BACKWARD: decreasing.
            return (a["_id"].Int() < b["_id"].Int()) ^
                (direction == CollectionScanParams::BACKWARD);
        });

        int expectedDocsTested = 0;
        std::vector<BSONObj> expectedDocs;
        // Per range: whether we have seen a document that overshoots that range. There can
        // only be at most one such overshoot per range.
        std::vector<bool> sawNextDocFor(rangeList.getRanges().size(), false);

        for (const auto& doc : sortedDocs) {
            RecordId rid = record_id_helpers::keyForElem(doc["_id"]);
            bool inAnyRange = false;
            for (size_t i = (direction == CollectionScanParams::FORWARD)
                     ? rangeList.getRanges().size() - 1
                     : 0;
                 i < rangeList.getRanges().size();
                 i += (direction == CollectionScanParams::FORWARD) ? -1 : 1) {
                const int compareRes = rangeList.getRanges()[i].compare(rid);
                if (0 == compareRes) {
                    inAnyRange = true;
                    break;
                }
                if (((direction == CollectionScanParams::FORWARD) ? 1 : -1) == compareRes) {
                    if (!sawNextDocFor[i]) {
                        sawNextDocFor[i] = true;
                        ++expectedDocsTested;
                    }
                    // This id fell after an earlier range before falling into a later one,
                    // so it cannot fall into any range.
                    // It also cannot cause any earlier range to be overshot.
                    break;
                }
            }
            if (!inAnyRange)
                continue;
            ++expectedDocsTested;
            if (!filter || exec::matcher::matchesBSON(filter, doc)) {
                expectedDocs.push_back(doc);
            }
        }

        ASSERT_EQ(stats->docsTested, (size_t)expectedDocsTested);
        ASSERT_EQ(actualDocs.size(), expectedDocs.size());
        for (size_t i = 0; i < actualDocs.size(); i++) {
            ASSERT_BSONOBJ_EQ(actualDocs[i], expectedDocs[i]);
        }

        // Expected seeks: one per range that has a bounded start.
        // FORWARD → start is min, BACKWARD → start is max.
        size_t expectedSeeks = 0;
        const bool forward = (direction == CollectionScanParams::FORWARD);
        for (const auto& range : rangeList.getRanges()) {
            if (forward ? range.getMin().has_value() : range.getMax().has_value())
                ++expectedSeeks;
        }
        ASSERT_EQ(stats->seeks, expectedSeeks);
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        ExpressionContextBuilder{}
            .opCtx(&_opCtx)
            .ns(NamespaceString::createNamespaceString_forTest("unittests.MultiRangeClusteredScan"))
            .build();

private:
    DBDirectClient _client;
};

// Forward/backward scan over multiple noncontiguous ranges.
TEST_F(MultiRangeClusteredScanTest, MultiRangeForward) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i < 30; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, true),      // [1,5]
        makeIntRange(10, false, 15, false),  // (10,15)
        makeIntRange(17, true, 17, true),    // [17,17]
        makeIntRange(20, true, 25, false),   // [20,25)
    });

    checkClusteredScanResult(ns, CollectionScanParams::FORWARD, rangeList, docs);
    checkClusteredScanResult(ns, CollectionScanParams::BACKWARD, rangeList, docs);
}

TEST_F(MultiRangeClusteredScanTest, EmptyRangeList) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    for (int i = 0; i < 10; ++i)
        insertDocument(ns, BSON("_id" << i));

    const auto coll = acquireCollectionMaybeLockFree(
        &_opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            &_opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
    ASSERT(coll.getCollectionPtr()->isClustered());

    MultiRangeClusteredScanParams params;
    params.rangeList = RecordIdRangeList::makeUnion({});  // Empty list (∅)

    WorkingSet ws;
    auto scan =
        std::make_unique<MultiRangeClusteredScan>(_expCtx.get(), coll, params, &ws, nullptr);

    if (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        ASSERT_EQ(scan->work(&id), PlanStage::IS_EOF);
        ASSERT(scan->isEOF());
    }

    auto* stats = dynamic_cast<const CollectionScanStats*>(scan->getSpecificStats());
    ASSERT_EQ(stats->seeks, 0u);
    ASSERT_EQ(stats->docsTested, 0u);
}

TEST_F(MultiRangeClusteredScanTest, MultiRangeSomeRangesEmpty) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    // _id 1..10 and 20..30 (a gap at 11..19).
    for (int i = 1; i <= 10; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }
    for (int i = 20; i <= 30; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, true),    // [1,5]  — has data
        makeIntRange(12, true, 15, true),  // [12,15] — no data
        makeIntRange(22, true, 28, true),  // [22,28] — has data
    });

    checkClusteredScanResult(ns, CollectionScanParams::FORWARD, rangeList, docs);
    checkClusteredScanResult(ns, CollectionScanParams::BACKWARD, rangeList, docs);
}

TEST_F(MultiRangeClusteredScanTest, MultiRangeExclusiveJunction) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i <= 12; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    // Excludes 5 (touches at 5 but exclusive on both sides).
    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, false),   // [1,5)
        makeIntRange(5, false, 10, true),  // (5,10]
    });
    ASSERT_EQ(rangeList.getRanges().size(), 2u);  // must not have merged

    checkClusteredScanResult(ns, CollectionScanParams::FORWARD, rangeList, docs);
    checkClusteredScanResult(ns, CollectionScanParams::BACKWARD, rangeList, docs);
}

TEST_F(MultiRangeClusteredScanTest, MultiRangeWithFilter) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i < 30; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 10, true),   // [1,10]
        makeIntRange(15, true, 25, true),  // [15,25]
    });

    // Keep only even _id values.
    StatusWithMatchExpression swMatch =
        MatchExpressionParser::parse(BSON("_id" << BSON("$mod" << BSON_ARRAY(2 << 0))), _expCtx);
    ASSERT_OK(swMatch.getStatus());
    auto filter = std::move(swMatch.getValue());

    checkClusteredScanResult(ns, CollectionScanParams::FORWARD, rangeList, docs, filter.get());
    checkClusteredScanResult(ns, CollectionScanParams::BACKWARD, rangeList, docs, filter.get());
}

// Verifies that the stage correctly handles a yield while a seek to the next range is pending.
TEST_F(MultiRangeClusteredScanTest, PendingSeekAcrossYield) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i <= 20; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    const auto coll = acquireCollectionMaybeLockFree(
        &_opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            &_opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
    ASSERT(coll.getCollectionPtr()->isClustered());

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, true),    // [1,5]
        makeIntRange(10, true, 15, true),  // [10,15]
    });

    MultiRangeClusteredScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.rangeList = rangeList;

    WorkingSet ws;
    auto scan =
        std::make_unique<MultiRangeClusteredScan>(_expCtx.get(), coll, params, &ws, nullptr);

    std::vector<BSONObj> results;
    bool yielded = false;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (state == PlanStage::ADVANCED) {
            results.push_back(ws.get(id)->doc.value().toBson().getOwned());
        } else if (state == PlanStage::NEED_TIME && !yielded) {
            // Save/restore around the inter-range gap (where _pendingSeek is true).
            scan->saveState();
            scan->restoreState(RestoreContext(nullptr));
            yielded = true;
        }
    }

    ASSERT(yielded) << "Expected at least one NEED_TIME between ranges";

    // Expect docs 1-5 then 10-15 in forward order.
    ASSERT_EQ(results.size(), 11);
    for (size_t i = 0; i < 5; i++) {
        ASSERT_BSONOBJ_EQ(results[i], docs[i + 1]);
    }
    for (size_t i = 5; i < 11; i++) {
        ASSERT_BSONOBJ_EQ(results[i], docs[i + 5]);
    }

    auto* stats = dynamic_cast<const CollectionScanStats*>(scan->getSpecificStats());
    ASSERT_EQ(stats->seeks, 2u);        // one seek per range with a bounded start
    ASSERT_EQ(stats->docsTested, 13u);  // 11 returned + one overshoot per range
}

// If the inter-range seek throws a WriteConflictException, the stage must not lose track of the
// pending seek — when the operation resumes after the yield, it has to retry the seek instead
// of falling back to next() on the cursor.
TEST_F(MultiRangeClusteredScanTest, PendingSeekSurvivesWriteConflict) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i <= 20; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    const auto coll = acquireCollectionMaybeLockFree(
        &_opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            &_opCtx, ns, AcquisitionPrerequisites::OperationType::kRead));
    ASSERT(coll.getCollectionPtr()->isClustered());

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, true),    // [1,5]
        makeIntRange(10, true, 15, true),  // [10,15]
    });

    MultiRangeClusteredScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.rangeList = rangeList;

    WorkingSet ws;
    auto scan =
        std::make_unique<MultiRangeClusteredScan>(_expCtx.get(), coll, params, &ws, nullptr);

    std::vector<BSONObj> results;
    // Drive work() until the stage reports NEED_TIME (the marker for `_pendingSeek`).
    bool reachedPendingSeek = false;
    while (!scan->isEOF() && !reachedPendingSeek) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (state == PlanStage::ADVANCED) {
            results.push_back(ws.get(id)->doc.value().toBson().getOwned());
        } else if (state == PlanStage::NEED_TIME) {
            reachedPendingSeek = true;
        }
    }
    ASSERT(reachedPendingSeek);
    // We should have collected the first range ([1,5]) before reaching the gap.
    ASSERT_EQ(results.size(), 5u);

    // Trip a WriteConflictException on the next storage read. The stage's inter-range
    // seek is the only pending read, so this targets it precisely. `nTimes=1` ensures that only
    // the inter-range seek throws — subsequent reads execute normally.
    {
        FailPointEnableBlock failPoint(
            "WTWriteConflictExceptionForReads",
            FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
        WorkingSetID id = WorkingSet::INVALID_ID;
        ASSERT_EQ(scan->work(&id), PlanStage::NEED_YIELD);
    }

    // The WCE rolled back the recovery unit's snapshot, so the next storage read must come after
    // a save/restore cycle (which abandons and re-acquires the snapshot).
    scan->saveState();
    scan->restoreState(RestoreContext(nullptr));

    // Now finish the scan.This `work()` call should retry the seek (landing at record 10),
    // then each subsequent work() advances through the second range with no
    // NEED_TIME / NEED_YIELD in between (there are no more gaps to leap over,
    // and the failpoint is off). After record 15 we read record 16 once to
    // detect we're past the end, and return IS_EOF.
    for (int i = 10; i <= 15; ++i) {
        ASSERT_FALSE(scan->isEOF());
        WorkingSetID id = WorkingSet::INVALID_ID;
        ASSERT_EQ(scan->work(&id), PlanStage::ADVANCED);
        results.push_back(ws.get(id)->doc.value().toBson().getOwned());
    }
    ASSERT_FALSE(scan->isEOF());
    {
        WorkingSetID id = WorkingSet::INVALID_ID;
        ASSERT_EQ(scan->work(&id), PlanStage::IS_EOF);
    }
    ASSERT(scan->isEOF());

    // Expect docs 1..5 then 10..15 in forward order — exactly the same set as if the WCE had
    // never happened.
    ASSERT_EQ(results.size(), 11u);
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_BSONOBJ_EQ(results[i], docs[i + 1]);
    }
    for (size_t i = 5; i < 11; ++i) {
        ASSERT_BSONOBJ_EQ(results[i], docs[i + 5]);
    }

    auto* stats = dynamic_cast<const CollectionScanStats*>(scan->getSpecificStats());
    // One initial seek + one inter-range seek (the retry after the WCE) = 2 seeks total.
    ASSERT_EQ(stats->seeks, 2u);
    // 11 returned + one overshoot per range = 13.
    ASSERT_EQ(stats->docsTested, 13u);
}

TEST_F(MultiRangeClusteredScanTest, MultiRangeUnboundedEnds) {
    auto ns = NamespaceString::createNamespaceString_forTest("a.b");
    auto collDeleter = createClusteredCollection(ns);
    std::vector<BSONObj> docs;
    for (int i = 0; i <= 20; ++i) {
        auto doc = BSON("_id" << i);
        insertDocument(ns, doc);
        docs.push_back(doc);
    }

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(boost::none, true, 5, false),   // (−∞, 5)
        makeIntRange(10, false, boost::none, true),  // (10, +∞)
    });
    ASSERT_EQ(rangeList.getRanges().size(), 2u);

    checkClusteredScanResult(ns, CollectionScanParams::FORWARD, rangeList, docs);
    checkClusteredScanResult(ns, CollectionScanParams::BACKWARD, rangeList, docs);
}

}  // namespace multi_range_clustered_scan_test
}  // namespace mongo

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/fetch.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/sort.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_yield_restore.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

/**
 * This file tests db/exec/sort.cpp
 */

namespace mongo {
namespace QueryStageSortTests {

class QueryStageSortTestBase {
public:
    QueryStageSortTestBase() : _client(&_opCtx) {}

    void fillData() {
        for (int i = 0; i < numObj(); ++i) {
            insert(BSON("foo" << i));
        }
    }

    virtual ~QueryStageSortTestBase() {
        _client.dropCollection(nss());
    }

    void insert(const BSONObj& obj) {
        _client.insert(nss(), obj);
    }

    void getRecordIds(std::set<RecordId>* out, const CollectionPtr& coll) {
        auto cursor = coll->getCursor(&_opCtx);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    /**
     * We feed a mix of (key, unowned, owned) data to the sort stage.
     */
    void insertVarietyOfObjects(WorkingSet* ws, QueuedDataStage* ms, const CollectionPtr& coll) {
        std::set<RecordId> recordIds;
        getRecordIds(&recordIds, coll);

        std::set<RecordId>::iterator it = recordIds.begin();

        for (int i = 0; i < numObj(); ++i, ++it) {
            ASSERT_FALSE(it == recordIds.end());

            // Insert some owned obj data.
            WorkingSetID id = ws->allocate();
            WorkingSetMember* member = ws->get(id);
            member->recordId = *it;
            auto snapshotBson = coll->docFor(&_opCtx, *it);
            member->doc = {snapshotBson.snapshotId(), Document{snapshotBson.value()}};
            ws->transitionToRecordIdAndObj(id);
            ms->pushBack(id);
        }
    }

    /*
     * Wraps a sort stage with a QueuedDataStage in a plan executor. Returns the plan executor,
     * which is owned by the caller.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutorWithSortStage(
        const CollectionAcquisition& coll) {
        // Build the mock scan stage which feeds the data.
        auto ws = std::make_unique<WorkingSet>();
        _workingSet = ws.get();
        auto queuedDataStage = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
        insertVarietyOfObjects(ws.get(), queuedDataStage.get(), coll.getCollectionPtr());

        auto sortPattern = BSON("foo" << 1);
        auto keyGenStage = std::make_unique<SortKeyGeneratorStage>(
            _expCtx, std::move(queuedDataStage), ws.get(), sortPattern);

        auto ss = std::make_unique<SortStageDefault>(_expCtx,
                                                     ws.get(),
                                                     SortPattern{sortPattern, _expCtx},
                                                     limit(),
                                                     maxMemoryUsageBytes(),
                                                     false,  // addSortKeyMetadata
                                                     std::move(keyGenStage));

        // The PlanExecutor will be automatically registered on construction due to the auto
        // yield policy, so it can receive invalidations when we remove documents later.
        auto statusWithPlanExecutor =
            plan_executor_factory::make(_expCtx,
                                        std::move(ws),
                                        std::move(ss),
                                        coll,
                                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                        QueryPlannerParams::DEFAULT);
        invariant(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.  Used to
    // normalize woCompare calls.
    int sgn(int i) {
        if (i == 0)
            return 0;
        return i > 0 ? 1 : -1;
    }

    /**
     * This method is called after we have produced a row of sorted output. We should be showing
     * memory usage to account for the rows buffered in memory.
     *
     * This method returns the new peak memory usage reported by the tracker, so that we can verify
     * the peak usage only goes up over time.
     */
    uint64_t checkMemoryTracking(const SimpleMemoryUsageTracker& tracker,
                                 uint64_t oldPeakMemBytes) {
        uint64_t inUseTrackedMemBytes = tracker.inUseTrackedMemoryBytes();
        // If we have processed any rows, we should have seen some memory usage.
        ASSERT_GT(inUseTrackedMemBytes, 0);

        uint64_t actualPeakMemBytes = tracker.peakTrackedMemoryBytes();
        ASSERT_GTE(actualPeakMemBytes, inUseTrackedMemBytes);
        ASSERT_GTE(actualPeakMemBytes, oldPeakMemBytes);
        return actualPeakMemBytes;
    }

    /**
     * A template used by many tests below.
     * Fill out numObj objects, sort them in the order provided by 'direction'.
     * If extAllowed is true, sorting will use use external sorting if available.
     * If limit is not zero, we limit the output of the sort stage to 'limit' results.
     *
     * Tests out both the default and "simple" sort.
     */
    void sortAndCheck(int direction, const CollectionAcquisition& coll) {
        // Test both default sort (which will use metadata if available) and simple sort, which
        // works with no metadata.
        doSortAndCheck<false /* default sort */>(direction, coll);
        doSortAndCheck<true /* simple sort */>(direction, coll);
    }

    template <bool useSortStageSimple>
    void doSortAndCheck(int direction, const CollectionAcquisition& coll) {
        auto ws = std::make_unique<WorkingSet>();
        auto queuedDataStage = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());

        // Insert a mix of the various types of data.
        insertVarietyOfObjects(ws.get(), queuedDataStage.get(), coll.getCollectionPtr());

        auto sortPattern = BSON("foo" << direction);
        std::unique_ptr<SortStage> sortStage;
        if constexpr (!useSortStageSimple) {
            auto keyGenStage = std::make_unique<SortKeyGeneratorStage>(
                _expCtx, std::move(queuedDataStage), ws.get(), sortPattern);

            sortStage = std::make_unique<SortStageDefault>(_expCtx,
                                                           ws.get(),
                                                           SortPattern{sortPattern, _expCtx},
                                                           limit(),
                                                           maxMemoryUsageBytes(),
                                                           false,  // addSortKeyMetadata
                                                           std::move(keyGenStage));
        } else {
            // Create a SortStageSimple instance. Skip the KeyGenStage, which creates the metadata
            // which is not needed for this case.
            sortStage = std::make_unique<SortStageSimple>(_expCtx,
                                                          ws.get(),
                                                          SortPattern{sortPattern, _expCtx},
                                                          limit(),
                                                          maxMemoryUsageBytes(),
                                                          false,  // addSortKeyMetadata
                                                          std::move(queuedDataStage));
        }
        // We use this below for showing we track memory as the query executes.
        const SimpleMemoryUsageTracker& memoryTracker = sortStage->getMemoryUsageTracker_forTest();

        auto fetchStage = std::make_unique<FetchStage>(
            _expCtx.get(), ws.get(), std::move(sortStage), nullptr, coll);

        // Must fetch so we can look at the doc as a BSONObj.
        auto statusWithPlanExecutor =
            plan_executor_factory::make(_expCtx,
                                        std::move(ws),
                                        std::move(fetchStage),
                                        coll,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Look at pairs of objects to make sure that the sort order is pairwise (and therefore
        // totally) correct.
        BSONObj last;
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&last, nullptr));
        last = last.getOwned();

        // Count 'last'.
        int count = 1;

        BSONObj current;
        PlanExecutor::ExecState state;
        uint64_t peakMemBytes = 0;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&current, nullptr))) {
            int cmp = sgn(::mongo::bson::compareObjectsAccordingToSort(current, last, sortPattern));
            // The next object should be equal to the previous or oriented according to the sort
            // pattern.
            ASSERT(cmp == 0 || cmp == 1);
            ++count;
            last = current.getOwned();

            peakMemBytes = checkMemoryTracking(memoryTracker, peakMemBytes);
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
        checkCount(count);

        ASSERT_EQUALS(memoryTracker.inUseTrackedMemoryBytes(), 0);
    }

    /**
     * Check number of results returned from sort.
     */
    void checkCount(int count) {
        // No limit, should get all objects back.
        // Otherwise, result set should be smaller of limit and input data size.
        if (limit() > 0 && limit() < numObj()) {
            ASSERT_EQUALS(limit(), count);
        } else {
            ASSERT_EQUALS(numObj(), count);
        }
    }

    virtual int numObj() = 0;

    // Returns sort limit
    // Leave as 0 to disable limit.
    virtual int limit() const {
        return 0;
    };

    uint64_t maxMemoryUsageBytes() const {
        return loadMemoryLimit(StageMemoryLimit::QueryMaxBlockingSortMemoryUsageBytes);
    }

    static const char* ns() {
        return "unittests.QueryStageSort";
    }
    static NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest(ns());
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    boost::intrusive_ptr<ExpressionContext> _expCtx =
        ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss()).build();
    DBDirectClient _client;
    WorkingSet* _workingSet = nullptr;
};


// Sort some small # of results in increasing order.
class QueryStageSortInc : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 100;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();

        fillData();
        sortAndCheck(1, coll);
    }
};

// Sort some small # of results in decreasing order.
class QueryStageSortDec : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 100;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();

        fillData();
        sortAndCheck(-1, coll);
    }
};

// Sort in descreasing order with limit applied
template <int LIMIT>
class QueryStageSortDecWithLimit : public QueryStageSortDec {
public:
    int limit() const override {
        return LIMIT;
    }
};

// Sort a big bunch of objects.
class QueryStageSortExt : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 10000;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();
        fillData();
        sortAndCheck(-1, coll);
    }
};

// Mutation invalidation of docs fed to sort.
class QueryStageSortMutationInvalidation : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 2000;
    }
    int limit() const override {
        return 10;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();

        fillData();

        // The data we're going to later invalidate.
        std::set<RecordId> recordIds;
        getRecordIds(&recordIds, coll.getCollectionPtr());

        auto exec = makePlanExecutorWithSortStage(coll);

        // This test is specifically for the classic PlanStage execution engine, so assert that we
        // have the right kind of PlanExecutor.
        auto execImpl = dynamic_cast<PlanExecutorImpl*>(exec.get());
        ASSERT(execImpl);

        SortStage* ss = static_cast<SortStageDefault*>(execImpl->getRootStage());
        SortKeyGeneratorStage* keyGenStage =
            static_cast<SortKeyGeneratorStage*>(ss->getChildren()[0].get());
        QueuedDataStage* queuedDataStage =
            static_cast<QueuedDataStage*>(keyGenStage->getChildren()[0].get());

        // Have sort read in data from the queued data stage.
        const int firstRead = 5;
        for (int i = 0; i < firstRead; ++i) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
        }

        // We should have read in the first 'firstRead' recordIds.  Invalidate the first one.
        // Since it's in the WorkingSet, the updates should not be reflected in the output.
        exec->saveState();
        std::set<RecordId>::iterator it = recordIds.begin();
        Snapshotted<BSONObj> oldDoc = coll.getCollectionPtr()->docFor(&_opCtx, *it);

        const OID updatedId = oldDoc.value().getField("_id").OID();
        const SnapshotId idBeforeUpdate = oldDoc.snapshotId();
        // We purposefully update the document to have a 'foo' value greater than limit().
        // This allows us to check that we don't return the new copy of a doc by asserting
        // foo < limit().
        auto newDoc = [&](const Snapshotted<BSONObj>& oldDoc) {
            return BSON("_id" << oldDoc.value()["_id"] << "foo" << limit() + 10);
        };
        CollectionUpdateArgs args{oldDoc.value()};
        {
            WriteUnitOfWork wuow(&_opCtx);
            collection_internal::updateDocument(&_opCtx,
                                                coll.getCollectionPtr(),
                                                *it,
                                                oldDoc,
                                                newDoc(oldDoc),
                                                collection_internal::kUpdateNoIndexes,
                                                nullptr /* indexesAffected */,
                                                nullptr /* opDebug */,
                                                &args);
            wuow.commit();
        }
        exec->restoreState(nullptr);

        // Read the rest of the data from the queued data stage.
        while (!queuedDataStage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ss->work(&id);
        }

        // Let's just invalidate everything now. Already read into ss, so original values
        // should be fetched.
        exec->saveState();
        while (it != recordIds.end()) {
            oldDoc = coll.getCollectionPtr()->docFor(&_opCtx, *it);
            {
                WriteUnitOfWork wuow(&_opCtx);
                collection_internal::updateDocument(&_opCtx,
                                                    coll.getCollectionPtr(),
                                                    *it++,
                                                    oldDoc,
                                                    newDoc(oldDoc),
                                                    collection_internal::kUpdateNoIndexes,
                                                    nullptr /* indexesAffected */,
                                                    nullptr /* opDebug */,
                                                    &args);
                wuow.commit();
            }
        }
        exec->restoreState(nullptr);

        // Verify that it's sorted, the right number of documents are returned, and they're all
        // in the expected range.
        int count = 0;
        int lastVal = 0;
        int thisVal;
        while (!ss->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            WorkingSetMember* member = _workingSet->get(id);
            ASSERT(member->hasObj());
            if (member->doc.value().getField("_id").getOid() == updatedId) {
                ASSERT(idBeforeUpdate == member->doc.snapshotId());
            }
            thisVal = member->doc.value().getField("foo").getInt();
            ASSERT_LTE(lastVal, thisVal);
            // Expect docs in range [0, limit)
            ASSERT_LTE(0, thisVal);
            ASSERT_LT(thisVal, limit());
            lastVal = thisVal;
            ++count;
        }
        // Returns all docs.
        ASSERT_EQUALS(limit(), count);
    }
};

// Deletion invalidation of everything fed to sort.
class QueryStageSortDeletionInvalidation : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 2000;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();

        fillData();

        // The data we're going to later invalidate.
        std::set<RecordId> recordIds;
        getRecordIds(&recordIds, coll.getCollectionPtr());

        auto exec = makePlanExecutorWithSortStage(coll);

        // This test is specifically for the classic PlanStage execution engine, so assert that we
        // have the right kind of PlanExecutor.
        auto execImpl = dynamic_cast<PlanExecutorImpl*>(exec.get());
        ASSERT(execImpl);

        SortStage* ss = static_cast<SortStageDefault*>(execImpl->getRootStage());
        SortKeyGeneratorStage* keyGenStage =
            static_cast<SortKeyGeneratorStage*>(ss->getChildren()[0].get());
        QueuedDataStage* queuedDataStage =
            static_cast<QueuedDataStage*>(keyGenStage->getChildren()[0].get());

        const int firstRead = 10;
        // Have sort read in data from the queued data stage.
        for (int i = 0; i < firstRead; ++i) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
        }

        // We should have read in the first 'firstRead' recordIds.  Invalidate the first.
        exec->saveState();
        OpDebug* const nullOpDebug = nullptr;
        std::set<RecordId>::iterator it = recordIds.begin();
        {
            WriteUnitOfWork wuow(&_opCtx);
            collection_internal::deleteDocument(
                &_opCtx, coll.getCollectionPtr(), kUninitializedStmtId, *it++, nullOpDebug);
            wuow.commit();
        }
        exec->restoreState(nullptr);

        // Read the rest of the data from the queued data stage.
        while (!queuedDataStage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ss->work(&id);
        }

        // Let's just invalidate everything now.
        exec->saveState();
        while (it != recordIds.end()) {
            {
                WriteUnitOfWork wuow(&_opCtx);
                collection_internal::deleteDocument(
                    &_opCtx, coll.getCollectionPtr(), kUninitializedStmtId, *it++, nullOpDebug);
                wuow.commit();
            }
        }
        exec->restoreState(nullptr);

        // Regardless of storage engine, all the documents should come back with their objects
        int count = 0;
        while (!ss->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            WorkingSetMember* member = _workingSet->get(id);
            ASSERT(member->hasObj());
            ++count;
        }

        // Returns all docs.
        ASSERT_EQUALS(limit() ? limit() : numObj(), count);
    }
};

// Deletion invalidation of everything fed to sort with limit enabled.
// Limit size of working set within sort stage to a small number
// Sort stage implementation should not try to invalidate RecordIds that
// are no longer in the working set.

template <int LIMIT>
class QueryStageSortDeletionInvalidationWithLimit : public QueryStageSortDeletionInvalidation {
public:
    int limit() const override {
        return LIMIT;
    }
};

// Should error out if we sort with parallel arrays.
class QueryStageSortParallelArrays : public QueryStageSortTestBase {
public:
    int numObj() override {
        return 100;
    }

    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        auto coll = ctx.getOrCreateCollection();

        auto ws = std::make_unique<WorkingSet>();
        auto queuedDataStage = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());

        for (int i = 0; i < numObj(); ++i) {
            {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->doc = {
                    SnapshotId(),
                    Document{fromjson("{a: [1,2,3], b:[1,2,3], c:[1,2,3], d:[1,2,3,4]}")}};
                member->transitionToOwnedObj();
                queuedDataStage->pushBack(id);
            }
            {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->doc = {SnapshotId(), Document{fromjson("{a:1, b:1, c:1}")}};
                member->transitionToOwnedObj();
                queuedDataStage->pushBack(id);
            }
        }

        auto sortPattern = BSON("b" << -1 << "c" << 1 << "a" << 1);

        auto keyGenStage = std::make_unique<SortKeyGeneratorStage>(
            _expCtx, std::move(queuedDataStage), ws.get(), sortPattern);

        auto sortStage = std::make_unique<SortStageDefault>(_expCtx,
                                                            ws.get(),
                                                            SortPattern{sortPattern, _expCtx},
                                                            0u,
                                                            maxMemoryUsageBytes(),
                                                            false,  // addSortKeyMetadata
                                                            std::move(keyGenStage));

        auto fetchStage = std::make_unique<FetchStage>(
            _expCtx.get(), ws.get(), std::move(sortStage), nullptr, coll);

        // We don't get results back since we're sorting some parallel arrays.
        auto statusWithPlanExecutor =
            plan_executor_factory::make(_expCtx,
                                        std::move(ws),
                                        std::move(fetchStage),
                                        coll,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT);
        auto exec = std::move(statusWithPlanExecutor.getValue());

        ASSERT_THROWS_CODE(exec->getNext(static_cast<BSONObj*>(nullptr), nullptr),
                           DBException,
                           ErrorCodes::BadValue);
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_sort") {}

    void setupTests() override {
        add<QueryStageSortInc>();
        add<QueryStageSortDec>();
        // Sort with limit has a general limiting strategy for limit > 1
        add<QueryStageSortDecWithLimit<10>>();
        // and a special case for limit == 1
        add<QueryStageSortDecWithLimit<1>>();
        add<QueryStageSortExt>();
        add<QueryStageSortMutationInvalidation>();
        add<QueryStageSortDeletionInvalidation>();
        add<QueryStageSortDeletionInvalidationWithLimit<10>>();
        add<QueryStageSortDeletionInvalidationWithLimit<1>>();
        add<QueryStageSortParallelArrays>();
    }
};

unittest::OldStyleSuiteInitializer<All> queryStageSortTest;

}  // namespace QueryStageSortTests
}  // namespace mongo

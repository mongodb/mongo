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

/**
 * This file tests the UpdateStage class
 */

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/eof.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/upsert_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace QueryStageUpdate {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.QueryStageUpdate");

class QueryStageUpdateBase {
public:
    QueryStageUpdateBase() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns_forTest());
        _client.dropCollection(nss);
        _client.createCollection(nss);
    }

    virtual ~QueryStageUpdateBase() {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns_forTest());
        _client.dropCollection(nss);
    }

    void insert(const BSONObj& doc) {
        _client.insert(nss, doc);
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss, obj);
    }

    size_t count(const BSONObj& query) {
        return _client.count(nss, query, 0, 0, 0);
    }

    std::unique_ptr<CanonicalQuery> canonicalize(const BSONObj& query) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(query);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(&_opCtx, *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    /**
     * Runs the update operation by calling work until EOF. Asserts that
     * the update stage always returns NEED_TIME.
     */
    void runUpdate(UpdateStage* updateStage) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (PlanStage::IS_EOF != state) {
            ASSERT_EQUALS(PlanStage::NEED_TIME, state);
            state = updateStage->work(&id);
        }
    }

    /**
     * Returns a vector of all of the documents currently in 'collection'.
     *
     * Uses a forward collection scan stage to get the docs, and populates 'out' with
     * the results.
     */
    void getCollContents(const CollectionAcquisition& collection, std::vector<BSONObj>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        std::unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                MONGO_verify(member->hasObj());
                out->push_back(member->doc.value().toBson().getOwned());
            }
        }
    }

    void getRecordIds(const CollectionAcquisition& collection,
                      CollectionScanParams::Direction direction,
                      std::vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        std::unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                MONGO_verify(member->hasRecordId());
                out->push_back(member->recordId);
            }
        }
    }

    /**
     * Asserts that 'objs' contains 'expectedDoc'.
     */
    void assertHasDoc(const std::vector<BSONObj>& objs, const BSONObj& expectedDoc) {
        bool foundDoc = false;
        for (size_t i = 0; i < objs.size(); i++) {
            if (0 == objs[i].woCompare(expectedDoc)) {
                foundDoc = true;
                break;
            }
        }
        ASSERT(foundDoc);
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss).build();

private:
    DBDirectClient _client;
};

/**
 * Test an upsert into an empty collection.
 */
class QueryStageUpdateUpsertEmptyColl : public QueryStageUpdateBase {
public:
    void run() {
        // Run the update.
        {
            const auto collection =
                acquireCollection(&_opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      &_opCtx, nss, AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            ASSERT(collection.exists());
            CurOp& curOp = *CurOp::get(_opCtx);
            OpDebug* opDebug = &curOp.debug();
            UpdateDriver driver(_expCtx);

            // Collection should be empty.
            ASSERT_EQUALS(0U, count(BSONObj()));

            auto request = UpdateRequest();
            request.setNamespaceString(nss);

            // Update is the upsert {_id: 0, x: 1}, {$set: {y: 2}}.
            BSONObj query = fromjson("{_id: 0, x: 1}");
            BSONObj updates = fromjson("{$set: {y: 2}}");

            request.setUpsert();
            request.setQuery(query);
            request.setUpdateModification(
                write_ops::UpdateModification::parseFromClassicUpdate(updates));
            request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

            const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
            const auto constants = boost::none;

            ASSERT_DOES_NOT_THROW(driver.parse(
                request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

            // Setup update params.
            UpdateStageParams params(&request, &driver, opDebug);
            std::unique_ptr<CanonicalQuery> cq(canonicalize(query));
            params.canonicalQuery = cq.get();

            auto ws = std::make_unique<WorkingSet>();
            auto eofStage =
                std::make_unique<EOFStage>(_expCtx.get(), eof_node::EOFType::NonExistentNamespace);

            auto updateStage = std::make_unique<UpsertStage>(
                _expCtx.get(), params, ws.get(), collection, eofStage.release());

            runUpdate(updateStage.get());
        }

        // Verify the contents of the resulting collection.
        {
            const auto collection =
                acquireCollection(&_opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      &_opCtx, nss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);

            std::vector<BSONObj> objs;
            getCollContents(collection, &objs);

            // Expect a single document, {_id: 0, x: 1, y: 2}.
            ASSERT_EQUALS(1U, objs.size());
            ASSERT_BSONOBJ_EQ(objs[0], fromjson("{_id: 0, x: 1, y: 2}"));
        }
    }
};

/**
 * Test the case in which the document about to updated is deleted.
 */
class QueryStageUpdateSkipDeletedDoc : public QueryStageUpdateBase {
public:
    void run() {
        // Run the update.
        {
            const auto collection =
                acquireCollection(&_opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      &_opCtx, nss, AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            ASSERT(collection.exists());

            // Populate the collection.
            for (int i = 0; i < 10; ++i) {
                insert(BSON("_id" << i << "foo" << i));
            }
            ASSERT_EQUALS(10U, count(BSONObj()));

            CurOp& curOp = *CurOp::get(_opCtx);
            OpDebug* opDebug = &curOp.debug();
            UpdateDriver driver(_expCtx);

            // Get the RecordIds that would be returned by an in-order scan.
            std::vector<RecordId> recordIds;
            getRecordIds(collection, CollectionScanParams::FORWARD, &recordIds);

            auto request = UpdateRequest();
            request.setNamespaceString(nss);

            // Update is a multi-update that sets 'bar' to 3 in every document
            // where foo is less than 5.
            BSONObj query = fromjson("{foo: {$lt: 5}}");
            BSONObj updates = fromjson("{$set: {bar: 3}}");

            request.setMulti();
            request.setQuery(query);
            request.setUpdateModification(
                write_ops::UpdateModification::parseFromClassicUpdate(updates));
            request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

            const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
            const auto constants = boost::none;

            ASSERT_DOES_NOT_THROW(driver.parse(
                request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

            // Configure the scan.
            CollectionScanParams collScanParams;
            collScanParams.direction = CollectionScanParams::FORWARD;
            collScanParams.tailable = false;

            // Configure the update.
            UpdateStageParams updateParams(&request, &driver, opDebug);
            std::unique_ptr<CanonicalQuery> cq(canonicalize(query));
            updateParams.canonicalQuery = cq.get();

            auto ws = std::make_unique<WorkingSet>();
            auto cs = std::make_unique<CollectionScan>(_expCtx.get(),
                                                       collection,
                                                       collScanParams,
                                                       ws.get(),
                                                       cq->getPrimaryMatchExpression());

            auto updateStage = std::make_unique<UpdateStage>(
                _expCtx.get(), updateParams, ws.get(), collection, cs.release());

            const UpdateStats* stats =
                static_cast<const UpdateStats*>(updateStage->getSpecificStats());

            const size_t targetDocIndex = 3;

            while (stats->nModified < targetDocIndex) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = updateStage->work(&id);
                ASSERT_EQUALS(PlanStage::NEED_TIME, state);
            }

            // Remove recordIds[targetDocIndex];
            static_cast<PlanStage*>(updateStage.get())->saveState();
            BSONObj targetDoc =
                collection.getCollectionPtr()->docFor(&_opCtx, recordIds[targetDocIndex]).value();
            ASSERT(!targetDoc.isEmpty());
            remove(targetDoc);
            static_cast<PlanStage*>(updateStage.get())
                ->restoreState(&collection.getCollectionPtr());

            // Do the remaining updates.
            while (!updateStage->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = updateStage->work(&id);
                ASSERT(PlanStage::NEED_TIME == state || PlanStage::IS_EOF == state);
            }

            // 4 of the 5 matching documents should have been modified (one was deleted).
            ASSERT_EQUALS(4U, stats->nModified);
            ASSERT_EQUALS(4U, stats->nMatched);
        }

        // Check the contents of the collection.
        {
            const auto collection =
                acquireCollection(&_opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      &_opCtx, nss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);

            std::vector<BSONObj> objs;
            getCollContents(collection, &objs);

            // Verify that the collection now has 9 docs (one was deleted).
            ASSERT_EQUALS(9U, objs.size());

            // Make sure that the collection has certain documents.
            assertHasDoc(objs, fromjson("{_id: 0, foo: 0, bar: 3}"));
            assertHasDoc(objs, fromjson("{_id: 1, foo: 1, bar: 3}"));
            assertHasDoc(objs, fromjson("{_id: 2, foo: 2, bar: 3}"));
            assertHasDoc(objs, fromjson("{_id: 4, foo: 4, bar: 3}"));
            assertHasDoc(objs, fromjson("{_id: 5, foo: 5}"));
            assertHasDoc(objs, fromjson("{_id: 6, foo: 6}"));
        }
    }
};

/**
 * Test that the update stage returns an owned copy of the original document if
 * ReturnDocOption::RETURN_OLD is specified.
 */
class QueryStageUpdateReturnOldDoc : public QueryStageUpdateBase {
public:
    void run() {
        // Populate the collection.
        for (int i = 0; i < 10; ++i) {
            insert(BSON("_id" << i << "foo" << i));
        }
        ASSERT_EQUALS(10U, count(BSONObj()));

        // Various variables we'll need.
        const auto collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest::fromOpCtx(&_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        ASSERT(collection.exists());
        OpDebug* opDebug = &CurOp::get(_opCtx)->debug();
        auto request = UpdateRequest();
        request.setNamespaceString(nss);
        UpdateDriver driver(_expCtx);
        const int targetDocIndex = 0;  // We'll be working with the first doc in the collection.
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = std::make_unique<WorkingSet>();
        const std::unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        std::vector<RecordId> recordIds;
        getRecordIds(collection, CollectionScanParams::FORWARD, &recordIds);

        // Populate the request.
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {x: 0}}")));
        request.setSort(BSONObj());
        request.setMulti(false);
        request.setReturnDocs(UpdateRequest::RETURN_OLD);
        request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
        const auto constants = boost::none;

        ASSERT_DOES_NOT_THROW(driver.parse(
            request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->recordId = recordIds[targetDocIndex];
        const BSONObj oldDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex);
        member->doc = {SnapshotId(), Document{oldDoc}};
        ws->transitionToRecordIdAndObj(id);
        qds->pushBack(id);

        // Configure the update.
        UpdateStageParams updateParams(&request, &driver, opDebug);
        updateParams.canonicalQuery = cq.get();

        const auto updateStage = std::make_unique<UpdateStage>(
            _expCtx.get(), updateParams, ws.get(), collection, qds.release());

        // Should return advanced.
        id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = updateStage->work(&id);
        ASSERT_EQUALS(PlanStage::ADVANCED, state);

        // Make sure the returned value is what we expect it to be.

        // Should give us back a valid id.
        ASSERT_TRUE(WorkingSet::INVALID_ID != id);
        WorkingSetMember* resultMember = ws->get(id);
        // With an owned copy of the object, with no RecordId.
        ASSERT_TRUE(resultMember->hasOwnedObj());
        ASSERT_FALSE(resultMember->hasRecordId());
        ASSERT_EQUALS(resultMember->getState(), WorkingSetMember::OWNED_OBJ);
        ASSERT_TRUE(resultMember->doc.value().isOwned());

        // Should be the old value.
        ASSERT_BSONOBJ_EQ(resultMember->doc.value().toBson(), oldDoc);

        // Should have done the update.
        BSONObj newDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex << "x" << 0);
        std::vector<BSONObj> objs;
        getCollContents(collection, &objs);
        ASSERT_BSONOBJ_EQ(objs[targetDocIndex], newDoc);

        // That should be it.
        id = WorkingSet::INVALID_ID;
        ASSERT_EQUALS(PlanStage::IS_EOF, updateStage->work(&id));
    }
};

/**
 * Test that the update stage returns an owned copy of the updated document if
 * ReturnDocOption::RETURN_NEW is specified.
 */
class QueryStageUpdateReturnNewDoc : public QueryStageUpdateBase {
public:
    void run() {
        // Populate the collection.
        for (int i = 0; i < 50; ++i) {
            insert(BSON("_id" << i << "foo" << i));
        }
        ASSERT_EQUALS(50U, count(BSONObj()));

        // Various variables we'll need.
        const auto collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest::fromOpCtx(&_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        ASSERT(collection.exists());
        OpDebug* opDebug = &CurOp::get(_opCtx)->debug();
        auto request = UpdateRequest();
        request.setNamespaceString(nss);
        UpdateDriver driver(_expCtx);
        const int targetDocIndex = 10;
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = std::make_unique<WorkingSet>();
        const std::unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        std::vector<RecordId> recordIds;
        getRecordIds(collection, CollectionScanParams::FORWARD, &recordIds);

        // Populate the request.
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {x: 0}}")));
        request.setSort(BSONObj());
        request.setMulti(false);
        request.setReturnDocs(UpdateRequest::RETURN_NEW);
        request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
        const auto constants = boost::none;

        ASSERT_DOES_NOT_THROW(driver.parse(
            request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->recordId = recordIds[targetDocIndex];
        const BSONObj oldDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex);
        member->doc = {SnapshotId(), Document{oldDoc}};
        ws->transitionToRecordIdAndObj(id);
        qds->pushBack(id);

        // Configure the update.
        UpdateStageParams updateParams(&request, &driver, opDebug);
        updateParams.canonicalQuery = cq.get();

        auto updateStage = std::make_unique<UpdateStage>(
            _expCtx.get(), updateParams, ws.get(), collection, qds.release());

        // Should return advanced.
        id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = updateStage->work(&id);
        ASSERT_EQUALS(PlanStage::ADVANCED, state);

        // Make sure the returned value is what we expect it to be.

        // Should give us back a valid id.
        ASSERT_TRUE(WorkingSet::INVALID_ID != id);
        WorkingSetMember* resultMember = ws->get(id);
        // With an owned copy of the object, with no RecordId.
        ASSERT_TRUE(resultMember->hasOwnedObj());
        ASSERT_FALSE(resultMember->hasRecordId());
        ASSERT_EQUALS(resultMember->getState(), WorkingSetMember::OWNED_OBJ);
        ASSERT_TRUE(resultMember->doc.value().isOwned());

        // Should be the new value.
        BSONObj newDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex << "x" << 0);
        ASSERT_BSONOBJ_EQ(resultMember->doc.value().toBson(), newDoc);

        // Should have done the update.
        std::vector<BSONObj> objs;
        getCollContents(collection, &objs);
        ASSERT_BSONOBJ_EQ(objs[targetDocIndex], newDoc);

        // That should be it.
        id = WorkingSet::INVALID_ID;
        ASSERT_EQUALS(PlanStage::IS_EOF, updateStage->work(&id));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_update") {}

    void setupTests() override {
        // Stage-specific tests below.
        add<QueryStageUpdateUpsertEmptyColl>();
        add<QueryStageUpdateSkipDeletedDoc>();
        add<QueryStageUpdateReturnOldDoc>();
        add<QueryStageUpdateReturnNewDoc>();
    }
};

unittest::OldStyleSuiteInitializer<All> all;

}  // namespace QueryStageUpdate
}  // namespace mongo

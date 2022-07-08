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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/upsert_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageUpdate {

using std::make_unique;
using std::unique_ptr;
using std::vector;

static const NamespaceString nss("unittests.QueryStageUpdate");

class QueryStageUpdateBase {
public:
    QueryStageUpdateBase() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.dropCollection(nss.ns());
        _client.createCollection(nss.ns());
    }

    virtual ~QueryStageUpdateBase() {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void insert(const BSONObj& doc) {
        _client.insert(nss.ns(), doc);
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss.ns(), obj);
    }

    size_t count(const BSONObj& query) {
        return _client.count(nss, query, 0, 0, 0);
    }

    unique_ptr<CanonicalQuery> canonicalize(const BSONObj& query) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(query);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        return std::move(statusWithCQ.getValue());
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
    void getCollContents(const CollectionPtr& collection, vector<BSONObj>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasObj());
                out->push_back(member->doc.value().toBson().getOwned());
            }
        }
    }

    void getRecordIds(const CollectionPtr& collection,
                      CollectionScanParams::Direction direction,
                      vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasRecordId());
                out->push_back(member->recordId);
            }
        }
    }

    /**
     * Asserts that 'objs' contains 'expectedDoc'.
     */
    void assertHasDoc(const vector<BSONObj>& objs, const BSONObj& expectedDoc) {
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
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss);

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
            dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
            CurOp& curOp = *CurOp::get(_opCtx);
            OpDebug* opDebug = &curOp.debug();
            UpdateDriver driver(_expCtx);
            CollectionPtr collection = ctx.getCollection();
            ASSERT(collection);

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

            const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
            const auto constants = boost::none;

            ASSERT_DOES_NOT_THROW(driver.parse(
                request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

            // Setup update params.
            UpdateStageParams params(&request, &driver, opDebug);
            unique_ptr<CanonicalQuery> cq(canonicalize(query));
            params.canonicalQuery = cq.get();

            auto ws = make_unique<WorkingSet>();
            auto eofStage = make_unique<EOFStage>(_expCtx.get());

            auto updateStage = make_unique<UpsertStage>(
                _expCtx.get(), params, ws.get(), collection, eofStage.release());

            runUpdate(updateStage.get());
        }

        // Verify the contents of the resulting collection.
        {
            AutoGetCollectionForReadCommand collection(&_opCtx, nss);

            vector<BSONObj> objs;
            getCollContents(collection.getCollection(), &objs);

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
            dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

            // Populate the collection.
            for (int i = 0; i < 10; ++i) {
                insert(BSON("_id" << i << "foo" << i));
            }
            ASSERT_EQUALS(10U, count(BSONObj()));

            CurOp& curOp = *CurOp::get(_opCtx);
            OpDebug* opDebug = &curOp.debug();
            UpdateDriver driver(_expCtx);
            CollectionPtr coll =
                CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, nss);
            ASSERT(coll);

            // Get the RecordIds that would be returned by an in-order scan.
            vector<RecordId> recordIds;
            getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

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
            unique_ptr<CanonicalQuery> cq(canonicalize(query));
            updateParams.canonicalQuery = cq.get();

            auto ws = make_unique<WorkingSet>();
            auto cs = make_unique<CollectionScan>(
                _expCtx.get(), coll, collScanParams, ws.get(), cq->root());

            auto updateStage =
                make_unique<UpdateStage>(_expCtx.get(), updateParams, ws.get(), coll, cs.release());

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
            BSONObj targetDoc = coll->docFor(&_opCtx, recordIds[targetDocIndex]).value();
            ASSERT(!targetDoc.isEmpty());
            remove(targetDoc);
            static_cast<PlanStage*>(updateStage.get())->restoreState(&coll);

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
            AutoGetCollectionForReadCommand collection(&_opCtx, nss);

            vector<BSONObj> objs;
            getCollContents(collection.getCollection(), &objs);

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
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        OpDebug* opDebug = &CurOp::get(_opCtx)->debug();
        const CollectionPtr& coll = ctx.getCollection();
        ASSERT(coll);
        auto request = UpdateRequest();
        request.setNamespaceString(nss);
        UpdateDriver driver(_expCtx);
        const int targetDocIndex = 0;  // We'll be working with the first doc in the collection.
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = make_unique<WorkingSet>();
        const unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Populate the request.
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {x: 0}}")));
        request.setSort(BSONObj());
        request.setMulti(false);
        request.setReturnDocs(UpdateRequest::RETURN_OLD);

        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
        const auto constants = boost::none;

        ASSERT_DOES_NOT_THROW(driver.parse(
            request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
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

        const auto updateStage =
            make_unique<UpdateStage>(_expCtx.get(), updateParams, ws.get(), coll, qds.release());

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
        vector<BSONObj> objs;
        getCollContents(coll, &objs);
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
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        OpDebug* opDebug = &CurOp::get(_opCtx)->debug();
        const CollectionPtr& coll = ctx.getCollection();
        ASSERT(coll);
        auto request = UpdateRequest();
        request.setNamespaceString(nss);
        UpdateDriver driver(_expCtx);
        const int targetDocIndex = 10;
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = make_unique<WorkingSet>();
        const unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Populate the request.
        request.setQuery(query);
        request.setUpdateModification(
            write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {x: 0}}")));
        request.setSort(BSONObj());
        request.setMulti(false);
        request.setReturnDocs(UpdateRequest::RETURN_NEW);

        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
        const auto constants = boost::none;

        ASSERT_DOES_NOT_THROW(driver.parse(
            request.getUpdateModification(), arrayFilters, constants, request.isMulti()));

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
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

        auto updateStage =
            make_unique<UpdateStage>(_expCtx.get(), updateParams, ws.get(), coll, qds.release());

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
        vector<BSONObj> objs;
        getCollContents(coll, &objs);
        ASSERT_BSONOBJ_EQ(objs[targetDocIndex], newDoc);

        // That should be it.
        id = WorkingSet::INVALID_ID;
        ASSERT_EQUALS(PlanStage::IS_EOF, updateStage->work(&id));
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_update") {}

    void setupTests() {
        // Stage-specific tests below.
        add<QueryStageUpdateUpsertEmptyColl>();
        add<QueryStageUpdateSkipDeletedDoc>();
        add<QueryStageUpdateReturnOldDoc>();
        add<QueryStageUpdateReturnNewDoc>();
    }
};

OldStyleSuiteInitializer<All> all;

}  // namespace QueryStageUpdate

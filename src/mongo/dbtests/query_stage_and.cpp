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
 * This file tests db/exec/and_*.cpp and RecordId invalidation.  RecordId invalidation forces a
 * fetch so we cannot test it outside of a dbtest.
 */


#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/str.h"

namespace QueryStageAnd {

using std::set;
using std::shared_ptr;
using std::unique_ptr;

class QueryStageAndBase {
public:
    QueryStageAndBase() : _client(&_opCtx) {}

    virtual ~QueryStageAndBase() {
        _client.dropCollection(nss());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), obj));
    }

    const IndexDescriptor* getIndex(const BSONObj& obj, const CollectionPtr& coll) {
        std::vector<const IndexDescriptor*> indexes;
        coll->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, obj, IndexCatalog::InclusionPolicy::kReady, &indexes);
        if (indexes.empty()) {
            FAIL(str::stream() << "Unable to find index with key pattern " << obj);
        }
        return indexes[0];
    }

    IndexScanParams makeIndexScanParams(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const IndexDescriptor* descriptor) {
        IndexScanParams params(opCtx, collection, descriptor);
        params.bounds.isSimpleRange = true;
        params.bounds.endKey = BSONObj();
        params.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        params.direction = 1;
        return params;
    }

    void getRecordIds(set<RecordId>* out, const CollectionPtr& coll) {
        auto cursor = coll->getCursor(&_opCtx);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    void insert(const BSONObj& obj) {
        _client.insert(nss(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss(), obj);
    }

    /**
     * Executes plan stage until EOF.  Returns number of results seen if execution reaches EOF
     * successfully. Throws on stage failure.
     */
    int countResults(PlanStage* stage) {
        int count = 0;
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            ++count;
        }
        return count;
    }

    /**
     * Gets the next result from 'stage'. Asserts that the returned working set member is fetched,
     * and that there are more results.
     */
    BSONObj getNext(PlanStage* stage, WorkingSet* ws) {
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);

            if (PlanStage::ADVANCED != status) {
                continue;
            }

            WorkingSetMember* member = ws->get(id);
            ASSERT(member->hasObj());
            return member->doc.value().toBson();
        }

        // We failed to produce a result.
        ASSERT(false);
        return BSONObj();
    }

    StringData ns() {
        return _nss.ns();
    }
    const NamespaceString& nss() {
        return _nss;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss());

private:
    DBDirectClient _client;
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("unittests.QueryStageAnd");
};

//
// Hash AND tests
//

/**
 * Delete a RecordId held by a hashed AND before the AND finishes evaluating. The AND should
 * return the result despite its deletion.
 */
class QueryStageAndHashDeleteDuringYield : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        Database* db = ctx.db();

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            db->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20.
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10.
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // 'ah' reads the first child into its hash table: foo=20, foo=19, ..., foo=0
        // in that order. Read half of them.
        for (int i = 0; i < 10; ++i) {
            WorkingSetID out;
            PlanStage::StageState status = ah->work(&out);
            ASSERT_EQUALS(PlanStage::NEED_TIME, status);
        }

        // Save state and delete one of the read objects.
        ah->saveState();
        set<RecordId> data;
        getRecordIds(&data, coll);
        size_t memUsageBefore = ah->getMemUsage();
        for (set<RecordId>::const_iterator it = data.begin(); it != data.end(); ++it) {
            if (coll->docFor(&_opCtx, *it).value()["foo"].numberInt() == 15) {
                remove(coll->docFor(&_opCtx, *it).value());
                break;
            }
        }
        size_t memUsageAfter = ah->getMemUsage();
        ah->restoreState(&coll);

        // The deleted result should still be buffered inside the AND_HASH stage, so there should be
        // no change in memory consumption.
        ASSERT_EQ(memUsageAfter, memUsageBefore);

        // Now, finish up the AND. We expect 10 results. Although the deleted result is still
        // buffered, the {bar: 1} index scan won't encounter the deleted document, and hence the
        // document won't appear in the result set.
        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            BSONElement elt;
            WorkingSetMember* member = ws.get(id);

            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_LESS_THAN_OR_EQUALS(elt.numberInt(), 20);
            ASSERT_NOT_EQUALS(15, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_GREATER_THAN_OR_EQUALS(elt.numberInt(), 10);
        }

        ASSERT_EQUALS(10, count);
    }
};

// Delete one of the "are we EOF?" lookahead results while the plan is yielded.
class QueryStageAndHashDeleteLookaheadDuringYield : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("_id" << i << "foo" << i << "bar" << i << "baz" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20 (descending).
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar <= 19 (descending).
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 19);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // First call to work reads the first result from the children. The first result for the
        // first scan over foo is {foo: 20, bar: 20, baz: 20}. The first result for the second scan
        // over bar is {foo: 19, bar: 19, baz: 19}.
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState status = ah->work(&id);
        ASSERT_EQUALS(PlanStage::NEED_TIME, status);

        // Delete 'deletedObj' from the collection.
        BSONObj deletedObj = BSON("_id" << 20 << "foo" << 20 << "bar" << 20 << "baz" << 20);
        ah->saveState();
        set<RecordId> data;
        getRecordIds(&data, coll);

        size_t memUsageBefore = ah->getMemUsage();
        for (auto&& recordId : data) {
            if (0 == deletedObj.woCompare(coll->docFor(&_opCtx, recordId).value())) {
                remove(coll->docFor(&_opCtx, recordId).value());
                break;
            }
        }

        // The deletion should not affect the amount of data buffered inside the AND_HASH stage.
        size_t memUsageAfter = ah->getMemUsage();
        ASSERT_EQUALS(memUsageBefore, memUsageAfter);

        ah->restoreState(&coll);

        // We expect that the deleted document doers not appear in our result set.
        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            WorkingSetMember* wsm = ws.get(id);
            ASSERT_NOT_EQUALS(0,
                              deletedObj.woCompare(coll->docFor(&_opCtx, wsm->recordId).value()));
            ++count;
        }

        ASSERT_EQUALS(count, 20);
    }
};

// An AND with two children.
class QueryStageAndHashTwoLeaf : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // foo == bar == baz, and foo<=20, bar>=10, so our values are:
        // foo == 10, 11, 12, 13, 14, 15. 16, 17, 18, 19, 20
        ASSERT_EQUALS(11, countResults(ah.get()));
    }
};

// An AND with two children.
// Add large keys (512 bytes) to index of first child to cause
// internal buffer within hashed AND to exceed threshold (32MB)
// before gathering all requested results.
class QueryStageAndHashTwoLeafFirstChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Generate large keys for {foo: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1 << "big" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        // Lower buffer limit to 20 * sizeof(big) to force memory error
        // before hashed AND is done reading the first child (stage has to
        // hold 21 keys in buffer for Foo <= 20).
        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws, 20 * big.size());

        // Foo <= 20
        auto params =
            makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1 << "big" << 1), coll));
        params.bounds.startKey = BSON("" << 20 << "" << big);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        ASSERT_THROWS_CODE(countResults(ah.get()),
                           DBException,
                           ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
    }
};

// An AND with three children.
// Add large keys (512 bytes) to index of last child to verify that
// keys in last child are not buffered
class QueryStageAndHashTwoLeafLastChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Generate large keys for {baz: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1 << "big" << 1));
        CollectionPtr coll = ctx.getCollection();

        // Lower buffer limit to 5 * sizeof(big) to ensure that
        // keys in last child's index are not buffered. There are 6 keys
        // that satisfy the criteria Foo <= 20 and Bar >= 10 and 5 <= baz <= 15.
        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws, 5 * big.size());

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1 << "big" << 1), coll));
        params.bounds.startKey = BSON("" << 10 << "" << big);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // foo == bar == baz, and foo<=20, bar>=10, so our values are:
        // foo == 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20.
        ASSERT_EQUALS(11, countResults(ah.get()));
    }
};

// An AND with three children.
class QueryStageAndHashThreeLeaf : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "baz" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // 5 <= baz <= 15
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("baz" << 1), coll));
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 15);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // foo == bar == baz, and foo<=20, bar>=10, 5<=baz<=15, so our values are:
        // foo == 10, 11, 12, 13, 14, 15.
        ASSERT_EQUALS(6, countResults(ah.get()));
    }
};

// An AND with three children.
// Add large keys (512 bytes) to index of second child to cause
// internal buffer within hashed AND to exceed threshold (32MB)
// before gathering all requested results.
// We need 3 children because the hashed AND stage buffered data for
// N-1 of its children. If the second child is the last child, it will not
// be buffered.
class QueryStageAndHashThreeLeafMiddleChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Generate large keys for {bar: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "baz" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1 << "big" << 1));
        addIndex(BSON("baz" << 1));
        CollectionPtr coll = ctx.getCollection();

        // Lower buffer limit to 10 * sizeof(big) to force memory error
        // before hashed AND is done reading the second child (stage has to
        // hold 11 keys in buffer for Foo <= 20 and Bar >= 10).
        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws, 10 * big.size());

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1 << "big" << 1), coll));
        params.bounds.startKey = BSON("" << 10 << "" << big);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // 5 <= baz <= 15
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("baz" << 1), coll));
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 15);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Stage execution should fail.
        ASSERT_THROWS_CODE(countResults(ah.get()),
                           DBException,
                           ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
    }
};

// An AND with an index scan that returns nothing.
class QueryStageAndHashWithNothing : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar == 5.  Index scan should be eof.
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 5);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        int count = 0;
        int works = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ++works;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            ++count;
        }

        ASSERT_EQUALS(0, count);

        // We check the "look ahead for EOF" here by examining the number of works required to
        // hit EOF.  Our first call to work will pick up that bar==5 is EOF and the AND will EOF
        // immediately.
        ASSERT_EQUALS(works, 1);
    }
};

// An AND that scans data but returns nothing.
class QueryStageAndHashProducesNothing : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 10; ++i) {
            insert(BSON("foo" << (100 + i)));
            insert(BSON("bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo >= 100
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 100);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar <= 100
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 100);
        // This is subtle and confusing.  We couldn't extract any keys from the elements with
        // 'foo' in them so we would normally index them with the "nothing found" key.  We don't
        // want to include that in our scan.
        params.bounds.endKey = BSON(""
                                    << "");
        params.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

/**
 * SERVER-14607: Check that hash-based intersection works when the first
 * child returns fetched docs but the second child returns index keys.
 */
class QueryStageAndHashFirstChildFetched : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        auto firstScan = std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr);

        // First child of the AND_HASH stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        auto fetch =
            std::make_unique<FetchStage>(_expCtx.get(), &ws, std::move(firstScan), nullptr, coll);
        ah->addChild(std::move(fetch));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Check that the AndHash stage returns docs {foo: 10, bar: 10}
        // through {foo: 20, bar: 20}.
        for (int i = 10; i <= 20; i++) {
            BSONObj obj = getNext(ah.get(), &ws);
            ASSERT_EQUALS(i, obj["foo"].numberInt());
            ASSERT_EQUALS(i, obj["bar"].numberInt());
        }
    }
};

/**
 * SERVER-14607: Check that hash-based intersection works when the first
 * child returns index keys but the second returns fetched docs.
 */
class QueryStageAndHashSecondChildFetched : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Foo <= 20
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar >= 10
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 10);
        auto secondScan = std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr);

        // Second child of the AND_HASH stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        auto fetch =
            std::make_unique<FetchStage>(_expCtx.get(), &ws, std::move(secondScan), nullptr, coll);
        ah->addChild(std::move(fetch));

        // Check that the AndHash stage returns docs {foo: 10, bar: 10}
        // through {foo: 20, bar: 20}.
        for (int i = 10; i <= 20; i++) {
            BSONObj obj = getNext(ah.get(), &ws);
            ASSERT_EQUALS(i, obj["foo"].numberInt());
            ASSERT_EQUALS(i, obj["bar"].numberInt());
        }
    }
};


class QueryStageAndHashDeadChild : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        Database* db = ctx.db();
        CollectionPtr coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_opCtx);
            coll = CollectionPtr(db->createCollection(&_opCtx, nss()));
            wuow.commit();
        }

        const BSONObj dataObj = fromjson("{'foo': 'bar'}");

        // Confirm exception is thrown when children contain the following WorkingSetMembers:
        //     Child1:  Data
        //     Child2:  NEED_TIME, FAILURE
        {
            WorkingSet ws;
            const auto andHashStage = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

            auto childStage1 = std::make_unique<MockStage>(_expCtx.get(), &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->doc = {SnapshotId(), Document{dataObj}};
                ws.transitionToRecordIdAndObj(id);
                childStage1->enqueueAdvanced(id);
            }

            auto childStage2 = std::make_unique<MockStage>(_expCtx.get(), &ws);
            childStage2->enqueueStateCode(PlanStage::NEED_TIME);
            childStage2->enqueueError(Status{ErrorCodes::InternalError, "mock error"});

            andHashStage->addChild(std::move(childStage1));
            andHashStage->addChild(std::move(childStage2));

            ASSERT_THROWS_CODE(
                getNext(andHashStage.get(), &ws), DBException, ErrorCodes::InternalError);
        }

        // Confirm exception is thrown when children contain the following WorkingSetMembers:
        //     Child1:  Data, FAILURE
        //     Child2:  Data
        {
            WorkingSet ws;
            const auto andHashStage = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

            auto childStage1 = std::make_unique<MockStage>(_expCtx.get(), &ws);

            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->doc = {SnapshotId(), Document{dataObj}};
                ws.transitionToRecordIdAndObj(id);
                childStage1->enqueueAdvanced(id);
            }
            childStage1->enqueueError(Status{ErrorCodes::InternalError, "mock error"});

            auto childStage2 = std::make_unique<MockStage>(_expCtx.get(), &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(2);
                wsm->doc = {SnapshotId(), Document{dataObj}};
                ws.transitionToRecordIdAndObj(id);
                childStage2->enqueueAdvanced(id);
            }

            andHashStage->addChild(std::move(childStage1));
            andHashStage->addChild(std::move(childStage2));

            ASSERT_THROWS_CODE(
                getNext(andHashStage.get(), &ws), DBException, ErrorCodes::InternalError);
        }

        // Confirm throws exception when children contain the following WorkingSetMembers:
        //     Child1:  Data
        //     Child2:  Data, FAILURE
        {
            WorkingSet ws;
            const auto andHashStage = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

            auto childStage1 = std::make_unique<MockStage>(_expCtx.get(), &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->doc = {SnapshotId(), Document{dataObj}};
                ws.transitionToRecordIdAndObj(id);
                childStage1->enqueueAdvanced(id);
            }

            auto childStage2 = std::make_unique<MockStage>(_expCtx.get(), &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(2);
                wsm->doc = {SnapshotId(), Document{dataObj}};
                ws.transitionToRecordIdAndObj(id);
                childStage2->enqueueAdvanced(id);
            }
            childStage2->enqueueError(Status{ErrorCodes::InternalError, "internal error"});

            andHashStage->addChild(std::move(childStage1));
            andHashStage->addChild(std::move(childStage2));

            ASSERT_THROWS_CODE(
                getNext(andHashStage.get(), &ws), DBException, ErrorCodes::InternalError);
        }
    }
};

//
// Sorted AND tests
//

/**
 * Delete a RecordId held by a sorted AND before the AND finishes evaluating.
 */
class QueryStageAndSortedDeleteDuringYield : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Insert a bunch of data.
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }
        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // Scan over foo == 1.
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Scan over bar == 1.
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Get the set of RecordIds in our collection to use later.
        set<RecordId> data;
        getRecordIds(&data, coll);

        // We're making an assumption here that happens to be true because we clear out the
        // collection before running this: increasing inserts have increasing RecordIds. This isn't
        // true in general if the collection is not dropped beforehand.
        WorkingSetID id = WorkingSet::INVALID_ID;

        // Sorted AND looks at the first child, which is an index scan over foo==1.
        ah->work(&id);

        // The first thing that the index scan returns (due to increasing RecordId trick) is the
        // very first insert, which should be the very first thing in data. Delete it.
        ah->saveState();
        remove(coll->docFor(&_opCtx, *data.begin()).value());
        ah->restoreState(&coll);

        auto it = data.begin();

        // Proceed along, AND-ing results.
        int count = 0;
        while (!ah->isEOF() && count < 10) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            ++it;
            WorkingSetMember* member = ws.get(id);

            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_EQUALS(member->recordId, *it);
        }

        // Move 'it' to a result that's yet to show up.
        for (int i = 0; i < count + 10; ++i) {
            ++it;
        }
        // Remove a result that's coming up.
        ah->saveState();
        remove(coll->docFor(&_opCtx, *it).value());
        ah->restoreState(&coll);

        // Get all results aside from the two we deleted.
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            WorkingSetMember* member = ws.get(id);

            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
        }

        ASSERT_EQUALS(count, 48);
    }
};


// An AND with three children.
class QueryStageAndSortedThreeLeaf : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            // Some data that'll show up but not be in all.
            insert(BSON("foo" << 1 << "baz" << 1));
            insert(BSON("foo" << 1 << "bar" << 1));
            // The needle in the haystack.  Only these should be returned by the AND.
            insert(BSON("foo" << 1 << "bar" << 1 << "baz" << 1));
            insert(BSON("foo" << 1));
            insert(BSON("bar" << 1));
            insert(BSON("baz" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // Scan over foo == 1
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // bar == 1
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // baz == 1
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("baz" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        ASSERT_EQUALS(50, countResults(ah.get()));
    }
};

// An AND with an index scan that returns nothing.
class QueryStageAndSortedWithNothing : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 8 << "bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // Foo == 7.  Should be EOF.
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 7);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Bar == 20, not EOF.
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 20);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

// An AND that scans data but returns nothing.
class QueryStageAndSortedProducesNothing : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            // Insert data with foo=7, bar==20, but nothing with both.
            insert(BSON("foo" << 8 << "bar" << 20));
            insert(BSON("foo" << 7 << "bar" << 21));
            insert(BSON("foo" << 7));
            insert(BSON("bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // foo == 7.
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 7);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // bar == 20.
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 20);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

// Verify that AND preserves the order of the last child.
class QueryStageAndSortedByLastChild : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        auto ah = std::make_unique<AndHashStage>(_expCtx.get(), &ws);

        // Scan over foo == 1
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // Intersect with 7 <= bar < 10000
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 10000);
        ah->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        WorkingSetID lastId = WorkingSet::INVALID_ID;

        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            BSONObj thisObj = coll->docFor(&_opCtx, ws.get(id)->recordId).value();
            ASSERT_EQUALS(7 + count, thisObj["bar"].numberInt());
            ++count;
            if (WorkingSet::INVALID_ID != lastId) {
                BSONObj lastObj = coll->docFor(&_opCtx, ws.get(lastId)->recordId).value();
                ASSERT_LESS_THAN(lastObj["bar"].woCompare(thisObj["bar"]), 0);
            }
            lastId = id;
        }

        ASSERT_EQUALS(count, 43);
    }
};

/**
 * SERVER-14607: Check that sort-based intersection works when the first
 * child returns fetched docs but the second child returns index keys.
 */
class QueryStageAndSortedFirstChildFetched : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        unique_ptr<AndSortedStage> as = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // Scan over foo == 1
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        auto firstScan = std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr);

        // First child of the AND_SORTED stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        auto fetch =
            std::make_unique<FetchStage>(_expCtx.get(), &ws, std::move(firstScan), nullptr, coll);
        as->addChild(std::move(fetch));

        // bar == 1
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        as->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        for (int i = 0; i < 50; i++) {
            BSONObj obj = getNext(as.get(), &ws);
            ASSERT_EQUALS(1, obj["foo"].numberInt());
            ASSERT_EQUALS(1, obj["bar"].numberInt());
        }
    }
};

/**
 * SERVER-14607: Check that sort-based intersection works when the first
 * child returns index keys but the second returns fetched docs.
 */
class QueryStageAndSortedSecondChildFetched : public QueryStageAndBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        if (!ctx.getCollection()) {
            WriteUnitOfWork wuow(&_opCtx);
            ctx.db()->createCollection(&_opCtx, nss());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        CollectionPtr coll = ctx.getCollection();

        WorkingSet ws;
        unique_ptr<AndSortedStage> as = std::make_unique<AndSortedStage>(_expCtx.get(), &ws);

        // Scan over foo == 1
        auto params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("foo" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        as->addChild(std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr));

        // bar == 1
        params = makeIndexScanParams(&_opCtx, coll, getIndex(BSON("bar" << 1), coll));
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        auto secondScan = std::make_unique<IndexScan>(_expCtx.get(), coll, params, &ws, nullptr);

        // Second child of the AND_SORTED stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        auto fetch =
            std::make_unique<FetchStage>(_expCtx.get(), &ws, std::move(secondScan), nullptr, coll);
        as->addChild(std::move(fetch));

        for (int i = 0; i < 50; i++) {
            BSONObj obj = getNext(as.get(), &ws);
            ASSERT_EQUALS(1, obj["foo"].numberInt());
            ASSERT_EQUALS(1, obj["bar"].numberInt());
        }
    }
};


class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_and") {}

    void setupTests() {
        add<QueryStageAndHashDeleteDuringYield>();
        add<QueryStageAndHashTwoLeaf>();
        add<QueryStageAndHashTwoLeafFirstChildLargeKeys>();
        add<QueryStageAndHashTwoLeafLastChildLargeKeys>();
        add<QueryStageAndHashThreeLeaf>();
        add<QueryStageAndHashThreeLeafMiddleChildLargeKeys>();
        add<QueryStageAndHashWithNothing>();
        add<QueryStageAndHashProducesNothing>();
        add<QueryStageAndHashDeleteLookaheadDuringYield>();
        add<QueryStageAndHashFirstChildFetched>();
        add<QueryStageAndHashSecondChildFetched>();
        add<QueryStageAndHashDeadChild>();
        add<QueryStageAndSortedDeleteDuringYield>();
        add<QueryStageAndSortedThreeLeaf>();
        add<QueryStageAndSortedWithNothing>();
        add<QueryStageAndSortedProducesNothing>();
        add<QueryStageAndSortedByLastChild>();
        add<QueryStageAndSortedFirstChildFetched>();
        add<QueryStageAndSortedSecondChildFetched>();
    }
};

OldStyleSuiteInitializer<All> queryStageAndAll;

}  // namespace QueryStageAnd

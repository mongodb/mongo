/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * This file tests db/exec/oplogstart.{h,cpp}. OplogStart is an execution stage
 * responsible for walking the oplog backwards in order to find where the oplog should
 * be replayed from for replication.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/oplogstart.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace OplogStartTests {

using std::unique_ptr;
using std::string;

static const NamespaceString nss("unittests.oplogstarttests");

class Base {
public:
    Base()
        : _scopedXact(&_txn, MODE_X),
          _lk(_txn.lockState()),
          _context(&_txn, nss.ns()),
          _client(&_txn) {
        Collection* c = _context.db()->getCollection(nss.ns());
        if (!c) {
            WriteUnitOfWork wuow(&_txn);
            c = _context.db()->createCollection(&_txn, nss.ns());
            wuow.commit();
        }
        ASSERT(c->getIndexCatalog()->haveIdIndex(&_txn));
    }

    ~Base() {
        client()->dropCollection(nss.ns());

        // The OplogStart stage is not allowed to outlive it's RecoveryUnit.
        _stage.reset();
    }

protected:
    Collection* collection() {
        return _context.db()->getCollection(nss.ns());
    }

    DBDirectClient* client() {
        return &_client;
    }

    void setupFromQuery(const BSONObj& query) {
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(query);
        auto statusWithCQ = CanonicalQuery::canonicalize(
            &_txn, std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        _cq = std::move(statusWithCQ.getValue());
        _oplogws.reset(new WorkingSet());
        _stage.reset(new OplogStart(&_txn, collection(), _cq->root(), _oplogws.get()));
    }

    void assertWorkingSetMemberHasId(WorkingSetID id, int expectedId) {
        WorkingSetMember* member = _oplogws->get(id);
        BSONElement idEl = member->obj.value()["_id"];
        ASSERT(!idEl.eoo());
        ASSERT(idEl.isNumber());
        ASSERT_EQUALS(idEl.numberInt(), expectedId);
    }

    unique_ptr<CanonicalQuery> _cq;
    unique_ptr<WorkingSet> _oplogws;
    unique_ptr<OplogStart> _stage;

private:
    // The order of these is important in order to ensure order of destruction
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    ScopedTransaction _scopedXact;
    Lock::GlobalWrite _lk;
    OldClientContext _context;

    DBDirectClient _client;
};


/**
 * When the ts is newer than the oldest document, the OplogStart
 * stage should find the oldest document using a backwards collection
 * scan.
 */
class OplogStartIsOldest : public Base {
public:
    void run() {
        for (int i = 0; i < 10; ++i) {
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << i));
        }

        setupFromQuery(BSON("ts" << BSON("$gte" << 10)));

        WorkingSetID id = WorkingSet::INVALID_ID;
        // collection scan needs to be initialized
        ASSERT_EQUALS(_stage->work(&id), PlanStage::NEED_TIME);
        // finds starting record
        ASSERT_EQUALS(_stage->work(&id), PlanStage::ADVANCED);
        ASSERT(_stage->isBackwardsScanning());

        assertWorkingSetMemberHasId(id, 9);
    }
};

/**
 * Find the starting oplog record by scanning backwards
 * all the way to the beginning.
 */
class OplogStartIsNewest : public Base {
public:
    void run() {
        for (int i = 0; i < 10; ++i) {
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << i));
        }

        setupFromQuery(BSON("ts" << BSON("$gte" << 1)));

        WorkingSetID id = WorkingSet::INVALID_ID;
        // collection scan needs to be initialized
        ASSERT_EQUALS(_stage->work(&id), PlanStage::NEED_TIME);
        // full collection scan back to the first oplog record
        for (int i = 0; i < 9; ++i) {
            ASSERT_EQUALS(_stage->work(&id), PlanStage::NEED_TIME);
            ASSERT(_stage->isBackwardsScanning());
        }
        ASSERT_EQUALS(_stage->work(&id), PlanStage::ADVANCED);

        assertWorkingSetMemberHasId(id, 0);
    }
};

/**
 * Find the starting oplog record by hopping to the
 * beginning of the extent.
 */
class OplogStartIsNewestExtentHop : public Base {
public:
    void run() {
        for (int i = 0; i < 10; ++i) {
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << i));
        }

        setupFromQuery(BSON("ts" << BSON("$gte" << 1)));

        WorkingSetID id = WorkingSet::INVALID_ID;
        // ensure that we go into extent hopping mode immediately
        _stage->setBackwardsScanTime(0);

        // We immediately switch to extent hopping mode, and
        // should find the beginning of the extent
        ASSERT_EQUALS(_stage->work(&id), PlanStage::ADVANCED);
        ASSERT(_stage->isExtentHopping());

        assertWorkingSetMemberHasId(id, 0);
    }
};

class SizedExtentHopBase : public Base {
public:
    SizedExtentHopBase() {
        client()->dropCollection(nss.ns());
    }
    virtual ~SizedExtentHopBase() {
        client()->dropCollection(nss.ns());
    }

    void run() {
        buildCollection();

        WorkingSetID id = WorkingSet::INVALID_ID;
        setupFromQuery(BSON("ts" << BSON("$gte" << tsGte())));

        // ensure that we go into extent hopping mode immediately
        _stage->setBackwardsScanTime(0);

        // hop back extent by extent
        for (int i = 0; i < numHops(); i++) {
            ASSERT_EQUALS(_stage->work(&id), PlanStage::NEED_TIME);
            ASSERT(_stage->isExtentHopping());
        }
        // find the right loc without hopping again
        ASSERT_EQUALS(_stage->work(&id), finalState());

        int startDocId = tsGte() - 1;
        if (startDocId >= 0) {
            assertWorkingSetMemberHasId(id, startDocId);
        }
    }

protected:
    void buildCollection() {
        BSONObj info;
        // Create a collection with specified extent sizes
        BSONObj command =
            BSON("create" << nss.coll() << "capped" << true << "$nExtents" << extentSizes()
                          << "autoIndexId"
                          << false);
        ASSERT(client()->runCommand(nss.db().toString(), command, info));

        // Populate documents.
        for (int i = 0; i < numDocs(); ++i) {
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << i << "payload" << payload8k()));
        }
    }

    static string payload8k() {
        return string(8 * 1024, 'a');
    }
    /** An extent of this size is too small to contain one document containing payload8k(). */
    static int tooSmall() {
        return 1 * 1024;
    }
    /** An extent of this size fits one document. */
    static int fitsOne() {
        return 10 * 1024;
    }
    /** An extent of this size fits many documents. */
    static int fitsMany() {
        return 50 * 1024;
    }

    // to be defined by subclasses
    virtual BSONArray extentSizes() const = 0;
    virtual int numDocs() const = 0;
    virtual int numHops() const = 0;
    virtual PlanStage::StageState finalState() const {
        return PlanStage::ADVANCED;
    }
    virtual int tsGte() const {
        return 1;
    }
};

/**
 * Test hopping over a single empty extent.
 *
 * Collection structure:
 *
 * [--- extent 0 --] [ ext 1 ] [--- extent 2 ---]
 * [ {_id: 0}      ] [<empty>] [ {_id: 1}       ]
 */
class OplogStartOneEmptyExtent : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 2;
    }
    virtual int numHops() const {
        return 1;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsOne() << tooSmall() << fitsOne());
    }
};

/**
 * Test hopping over two consecutive empty extents.
 *
 * Collection structure:
 *
 * [--- extent 0 --] [ ext 1 ] [ ext 2 ] [--- extent 3 ---]
 * [ {_id: 0}      ] [<empty>] [<empty>] [ {_id: 1}       ]
 */
class OplogStartTwoEmptyExtents : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 2;
    }
    virtual int numHops() const {
        return 1;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsOne() << tooSmall() << tooSmall() << fitsOne());
    }
};

/**
 * Two extents, each filled with several documents. This
 * should require us to make just a single extent hop.
 */
class OplogStartTwoFullExtents : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 10;
    }
    virtual int numHops() const {
        return 1;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsMany() << fitsMany());
    }
};

/**
 * Four extents in total. Three are populated with multiple
 * documents, but one of the middle extents is empty. This
 * should require two extent hops.
 */
class OplogStartThreeFullOneEmpty : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 14;
    }
    virtual int numHops() const {
        return 2;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsMany() << fitsMany() << tooSmall() << fitsMany());
    }
};

/**
 * Test that extent hopping mode works properly in the
 * special case of one extent.
 */
class OplogStartOneFullExtent : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 4;
    }
    virtual int numHops() const {
        return 0;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsMany());
    }
};

/**
 * Collection structure:
 *
 * [ ext 0 ] [--- extent 1 --] [--- extent 2 ---]
 * [<empty>] [ {_id: 0}      ] [ {_id: 1}       ]
 */
class OplogStartFirstExtentEmpty : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 2;
    }
    virtual int numHops() const {
        return 1;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(tooSmall() << fitsOne() << fitsOne());
    }
};

/**
 * Find that we need to start from the very beginning of
 * the collection (the EOF case), after extent hopping
 * to the beginning.
 *
 * This requires two hops: one between the two extents,
 * and one to hop back to the "null extent" which precedes
 * the first extent.
 */
class OplogStartEOF : public SizedExtentHopBase {
    virtual int numDocs() const {
        return 2;
    }
    virtual int numHops() const {
        return 2;
    }
    virtual BSONArray extentSizes() const {
        return BSON_ARRAY(fitsOne() << fitsOne());
    }
    virtual PlanStage::StageState finalState() const {
        return PlanStage::IS_EOF;
    }
    virtual int tsGte() const {
        return 0;
    }
};

class All : public Suite {
public:
    All() : Suite("oplogstart") {}

    void setupTests() {
        add<OplogStartIsOldest>();
        add<OplogStartIsNewest>();

        // These tests rely on extent allocation details specific to mmapv1.
        // TODO figure out a way to generically test this.
        if (getGlobalServiceContext()->getGlobalStorageEngine()->isMmapV1()) {
            add<OplogStartIsNewestExtentHop>();
            add<OplogStartOneEmptyExtent>();
            add<OplogStartTwoEmptyExtents>();
            add<OplogStartTwoFullExtents>();
            add<OplogStartThreeFullOneEmpty>();
            add<OplogStartOneFullExtent>();
            add<OplogStartFirstExtentEmpty>();
            add<OplogStartEOF>();
        }
    }
};

SuiteInstance<All> oplogStart;

}  // namespace OplogStartTests

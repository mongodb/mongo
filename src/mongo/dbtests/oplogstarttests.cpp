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
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/oplogstart.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace OplogStartTests {

using std::unique_ptr;
using std::string;

static const NamespaceString nss("unittests.oplogstarttests");

class Base {
public:
    Base() : _lk(&_opCtx), _context(&_opCtx, nss.ns()), _client(&_opCtx) {
        Collection* c = _context.db()->getCollection(&_opCtx, nss);
        if (!c) {
            WriteUnitOfWork wuow(&_opCtx);
            c = _context.db()->createCollection(&_opCtx, nss.ns());
            wuow.commit();
        }
        ASSERT(c->getIndexCatalog()->haveIdIndex(&_opCtx));
    }

    ~Base() {
        client()->dropCollection(nss.ns());

        // The OplogStart stage is not allowed to outlive it's RecoveryUnit.
        _stage.reset();
    }

protected:
    Collection* collection() {
        return _context.db()->getCollection(&_opCtx, nss);
    }

    DBDirectClient* client() {
        return &_client;
    }

    void setupFromQuery(const BSONObj& query) {
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(query);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(qr));
        ASSERT_OK(statusWithCQ.getStatus());
        _cq = std::move(statusWithCQ.getValue());
        _oplogws.reset(new WorkingSet());
        const auto timestamp = query[repl::OpTime::kTimestampFieldName]
                                   .embeddedObjectUserCheck()
                                   .firstElement()
                                   .timestamp();
        _stage = stdx::make_unique<OplogStart>(&_opCtx, collection(), timestamp, _oplogws.get());
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
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
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
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << Timestamp(1000, i)));
        }

        setupFromQuery(BSON("ts" << BSON("$gte" << Timestamp(1000, 10))));

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
            client()->insert(nss.ns(), BSON("_id" << i << "ts" << Timestamp(1000, i)));
        }

        setupFromQuery(BSON("ts" << BSON("$gte" << Timestamp(1000, 0))));

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

class All : public Suite {
public:
    All() : Suite("oplogstart") {}

    void setupTests() {
        // Replication is not supported by mobile SE.
        if (mongo::storageGlobalParams.engine == "mobile") {
            return;
        }
        add<OplogStartIsOldest>();
        add<OplogStartIsNewest>();
    }
};

SuiteInstance<All> oplogStart;

}  // namespace OplogStartTests

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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * This file tests that a manually yielded PlanExecutor will still receive invalidations and be
 * marked as killed by events like collection drops.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace ExecutorRegistry {

using std::unique_ptr;

static const NamespaceString nss("unittests.ExecutorRegistryDiskLocInvalidation");

class ExecutorRegistryBase {
public:
    ExecutorRegistryBase() : _client(&_opCtx) {
        _ctx.reset(new OldClientWriteContext(&_opCtx, nss.ns()));
        _client.dropCollection(nss.ns());

        for (int i = 0; i < N(); ++i) {
            _client.insert(nss.ns(), BSON("foo" << i));
        }
    }

    /**
     * Return a plan executor that is going over the collection in nss.ns().
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollscan() {
        unique_ptr<WorkingSet> ws(new WorkingSet());
        CollectionScanParams params;
        params.collection = collection();
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;
        unique_ptr<CollectionScan> scan(new CollectionScan(&_opCtx, params, ws.get(), NULL));

        // Create a plan executor to hold it
        auto qr = stdx::make_unique<QueryRequest>(nss);
        auto statusWithCQ = CanonicalQuery::canonicalize(
            &_opCtx, std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Takes ownership of 'ws', 'scan', and 'cq'.
        auto statusWithPlanExecutor = PlanExecutor::make(&_opCtx,
                                                         std::move(ws),
                                                         std::move(scan),
                                                         std::move(cq),
                                                         _ctx->db()->getCollection(&_opCtx, nss),
                                                         PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

    int N() {
        return 50;
    }

    Collection* collection() {
        return _ctx->db()->getCollection(&_opCtx, nss);
    }

    // Order of these is important for initialization
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    unique_ptr<OldClientWriteContext> _ctx;
    DBDirectClient _client;
};


// Test that a registered PlanExecutor receives invalidation notifications.
class ExecutorRegistryDiskLocInvalid : public ExecutorRegistryBase {
public:
    void run() {
        if (supportsDocLocking()) {
            return;
        }

        auto exec = getCollscan();
        BSONObj obj;

        // Read some of it.
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        // Register it.
        exec->saveState();
        // At this point it's safe to yield.  forceYield would do that.  Let's now simulate some
        // stuff going on in the yield.

        // Delete some data, namely the next 2 things we'd expect.
        _client.remove(nss.ns(), BSON("foo" << 10));
        _client.remove(nss.ns(), BSON("foo" << 11));

        // At this point, we're done yielding.  We recover our lock.

        // And clean up anything that happened before.
        exec->restoreState();

        // Make sure that the PlanExecutor moved forward over the deleted data.  We don't see
        // foo==10
        // or foo==11.
        for (int i = 12; i < N(); ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&obj, NULL));
    }
};

// Test that registered PlanExecutors are killed when their collection is dropped.
class ExecutorRegistryDropCollection : public ExecutorRegistryBase {
public:
    void run() {
        auto exec = getCollscan();
        BSONObj obj;

        // Read some of it.
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        // Save state and register.
        exec->saveState();

        // Drop a collection that's not ours.
        _client.dropCollection("unittests.someboguscollection");

        // Unregister and restore state.
        exec->restoreState();

        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(10, obj["foo"].numberInt());

        exec->saveState();

        _client.dropCollection(nss.ns());

        exec->restoreState();

        // PlanExecutor was killed.
        ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&obj, NULL));
    }
};

// Test that registered PlanExecutors are killed when all indices are dropped on the collection.
class ExecutorRegistryDropAllIndices : public ExecutorRegistryBase {
public:
    void run() {
        auto exec = getCollscan();
        BSONObj obj;

        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), BSON("foo" << 1)));

        // Read some of it.
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        exec->saveState();
        _client.dropIndexes(nss.ns());
        exec->restoreState();
        ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&obj, NULL));
    }
};

// Test that registered PlanExecutors are killed when an index is dropped on the collection.
class ExecutorRegistryDropOneIndex : public ExecutorRegistryBase {
public:
    void run() {
        auto exec = getCollscan();
        BSONObj obj;

        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), BSON("foo" << 1)));

        // Read some of it.
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        exec->saveState();
        _client.dropIndex(nss.ns(), BSON("foo" << 1));
        exec->restoreState();
        ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&obj, NULL));
    }
};

// Test that registered PlanExecutors are killed when their database is dropped.
class ExecutorRegistryDropDatabase : public ExecutorRegistryBase {
public:
    void run() {
        auto exec = getCollscan();
        BSONObj obj;

        // Read some of it.
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
        }

        exec->saveState();

        // Drop a DB that's not ours.  We can't have a lock at all to do this as dropping a DB
        // requires a "global write lock."
        _ctx.reset();
        _client.dropDatabase("somesillydb");
        _ctx.reset(new OldClientWriteContext(&_opCtx, nss.ns()));
        exec->restoreState();

        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(10, obj["foo"].numberInt());

        exec->saveState();

        // Drop our DB.  Once again, must give up the lock.
        _ctx.reset();
        _client.dropDatabase("unittests");
        _ctx.reset(new OldClientWriteContext(&_opCtx, nss.ns()));
        exec->restoreState();
        _ctx.reset();

        // PlanExecutor was killed.
        ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&obj, NULL));
    }
};

// TODO: Test that this works with renaming a collection.

class All : public Suite {
public:
    All() : Suite("executor_registry") {}

    void setupTests() {
        add<ExecutorRegistryDiskLocInvalid>();
        add<ExecutorRegistryDropCollection>();
        add<ExecutorRegistryDropAllIndices>();
        add<ExecutorRegistryDropOneIndex>();
        add<ExecutorRegistryDropDatabase>();
    }
};

SuiteInstance<All> executorRegistryAll;

}  // namespace ExecutorRegistry

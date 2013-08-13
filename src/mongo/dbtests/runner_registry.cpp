/**
 *    Copyright (C) 2013 10gen Inc.
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
 * This file tests Runner forced yielding, ClientCursor::registerRunner, and
 * ClientCursor::deregisterRunner.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/dbtests/dbtests.h"


namespace RunnerRegistry {

    class RunnerRegistryBase {
    public:
        RunnerRegistryBase() {
            _ctx.reset(new Client::WriteContext(ns()));
            _client.dropCollection(ns());

            for (int i = 0; i < N(); ++i) {
                _client.insert(ns(), BSON("foo" << i));
            }
        }

        /**
         * Return a runner that is going over the collection in ns().
         */
        Runner* getCollscan() {
            auto_ptr<WorkingSet> ws(new WorkingSet());
            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;
            auto_ptr<CollectionScan> scan(new CollectionScan(params, ws.get(), NULL));

            // Create a runner to hold it
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns(), BSONObj(), &cq).isOK());
            // Owns all args
            auto_ptr<Runner> run(new SingleSolutionRunner(cq, NULL, scan.release(), ws.release()));
            return run.release();
        }

        int N() { return 50; }

        static const char* ns() { return "unittests.RunnerRegistryDiskLocInvalidation"; }
        static DBDirectClient _client;
        auto_ptr<Client::WriteContext> _ctx;
    };

    DBDirectClient RunnerRegistryBase::_client;

    // Test that a registered runner receives invalidation notifications.
    class RunnerRegistryDiskLocInvalid : public RunnerRegistryBase {
    public:
        void run() {
            auto_ptr<Runner> run(getCollscan());
            BSONObj obj;

            // Read some of it.
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            // Register it.
            run->saveState();
            ClientCursor::registerRunner(run.get());
            // At this point it's safe to yield.  forceYield would do that.  Let's now simulate some
            // stuff going on in the yield.

            // Delete some data, namely the next 2 things we'd expect.
            _client.remove(ns(), BSON("foo" << 10));
            _client.remove(ns(), BSON("foo" << 11));

            // At this point, we're done yielding.  We recover our lock.

            // Unregister the runner.
            ClientCursor::deregisterRunner(run.get());

            // And clean up anything that happened before.
            run->restoreState();

            // Make sure that the runner moved forward over the deleted data.  We don't see foo==10
            // or foo==11.
            for (int i = 12; i < N(); ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            ASSERT_EQUALS(Runner::RUNNER_EOF, run->getNext(&obj, NULL));
        }
    };

    // Test that registered runners are killed when their collection is dropped.
    class RunnerRegistryDropCollection : public RunnerRegistryBase {
    public:
        void run() {
            auto_ptr<Runner> run(getCollscan());
            BSONObj obj;

            // Read some of it.
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop a collection that's not ours.
            _client.dropCollection("unittests.someboguscollection");

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
            ASSERT_EQUALS(10, obj["foo"].numberInt());

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop our collection.
            _client.dropCollection(ns());

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            // Runner was killed.
            ASSERT_EQUALS(Runner::RUNNER_DEAD, run->getNext(&obj, NULL));
        }
    };

    // Test that registered runners are killed when all indices are dropped on the collection.
    class RunnerRegistryDropAllIndices : public RunnerRegistryBase {
    public:
        void run() {
            auto_ptr<Runner> run(getCollscan());
            BSONObj obj;

            _client.ensureIndex(ns(), BSON("foo" << 1));

            // Read some of it.
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop all indices.
            _client.dropIndexes(ns());

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            // Runner was killed.
            ASSERT_EQUALS(Runner::RUNNER_DEAD, run->getNext(&obj, NULL));
        }
    };

    // Test that registered runners are killed when an index is dropped on the collection.
    class RunnerRegistryDropOneIndex : public RunnerRegistryBase {
    public:
        void run() {
            auto_ptr<Runner> run(getCollscan());
            BSONObj obj;

            _client.ensureIndex(ns(), BSON("foo" << 1));

            // Read some of it.
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop a specific index.
            _client.dropIndex(ns(), BSON("foo" << 1));

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            // Runner was killed.
            ASSERT_EQUALS(Runner::RUNNER_DEAD, run->getNext(&obj, NULL));
        }
    };

    // Test that registered runners are killed when their database is dropped.
    class RunnerRegistryDropDatabase : public RunnerRegistryBase {
    public:
        void run() {
            auto_ptr<Runner> run(getCollscan());
            BSONObj obj;

            // Read some of it.
            for (int i = 0; i < 10; ++i) {
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
            }

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop a DB that's not ours.  We can't have a lock at all to do this as dropping a DB
            // requires a "global write lock."
            _ctx.reset();
            _client.dropDatabase("somesillydb");
            _ctx.reset(new Client::WriteContext(ns()));

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            ASSERT_EQUALS(Runner::RUNNER_ADVANCED, run->getNext(&obj, NULL));
            ASSERT_EQUALS(10, obj["foo"].numberInt());

            // Save state and register.
            run->saveState();
            ClientCursor::registerRunner(run.get());

            // Drop our DB.  Once again, must give up the lock.
            _ctx.reset();
            _client.dropDatabase("unittests");
            _ctx.reset(new Client::WriteContext(ns()));

            // Unregister and restore state.
            ClientCursor::deregisterRunner(run.get());
            run->restoreState();

            // Runner was killed.
            ASSERT_EQUALS(Runner::RUNNER_DEAD, run->getNext(&obj, NULL));
        }
    };

    // TODO: Test that this works with renaming a collection.

    class All : public Suite {
    public:
        All() : Suite( "runner_registry" ) { }

        void setupTests() {
            add<RunnerRegistryDiskLocInvalid>();
            add<RunnerRegistryDropCollection>();
            add<RunnerRegistryDropAllIndices>();
            add<RunnerRegistryDropOneIndex>();
            add<RunnerRegistryDropDatabase>();
        }
    }  runnerRegistryAll;

}  // namespace RunnerRegistry

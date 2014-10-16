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

#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryPlanExecutor {

    class PlanExecutorBase {
    public:
        PlanExecutorBase() : _client(&_txn) {

        }

        virtual ~PlanExecutorBase() {
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        void dropCollection() {
            _client.dropCollection(ns());
        }

        void update(BSONObj& query, BSONObj& updateSpec) {
            _client.update(ns(), query, updateSpec, false, false);
        }

        /**
         * Given a match expression, represented as the BSON object 'filterObj',
         * create a PlanExecutor capable of executing a simple collection
         * scan.
         *
         * The caller takes ownership of the returned PlanExecutor*.
         */
        PlanExecutor* makeCollScanExec(Collection* coll, BSONObj& filterObj) {
            CollectionScanParams csparams;
            csparams.collection = coll;
            csparams.direction = CollectionScanParams::FORWARD;
            auto_ptr<WorkingSet> ws(new WorkingSet());

            // Canonicalize the query
            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns(), filterObj, &cq).isOK());
            verify(NULL != cq);

            // Make the stage.
            auto_ptr<PlanStage> root(new CollectionScan(&_txn, csparams, ws.get(), cq->root()));

            // Hand the plan off to the executor.
            PlanExecutor* exec = new PlanExecutor(&_txn, ws.release(), root.release(), cq, coll);
            return exec;
        }

        /**
         * @param indexSpec -- a BSONObj giving the index over which to
         *   scan, e.g. {_id: 1}.
         * @param start -- the lower bound (inclusive) at which to start
         *   the index scan
         * @param end -- the lower bound (inclusive) at which to end the
         *   index scan
         *
         * Returns a PlanExecutor capable of executing an index scan
         * over the specified index with the specified bounds.
         *
         * The caller takes ownership of the returned PlanExecutor*.
         */
        PlanExecutor* makeIndexScanExec(Client::Context& context,
                                                BSONObj& indexSpec, int start, int end) {
            // Build the index scan stage.
            IndexScanParams ixparams;
            ixparams.descriptor = getIndex(context.db(), indexSpec);
            ixparams.bounds.isSimpleRange = true;
            ixparams.bounds.startKey = BSON("" << start);
            ixparams.bounds.endKey = BSON("" << end);
            ixparams.bounds.endKeyInclusive = true;
            ixparams.direction = 1;

            const Collection* coll = context.db()->getCollection(&_txn, ns());

            auto_ptr<WorkingSet> ws(new WorkingSet());
            IndexScan* ix = new IndexScan(&_txn, ixparams, ws.get(), NULL);
            auto_ptr<PlanStage> root(new FetchStage(&_txn, ws.get(), ix, NULL, coll));

            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns(), BSONObj(), &cq).isOK());
            verify(NULL != cq);

            // Hand the plan off to the executor.
            return new PlanExecutor(&_txn, ws.release(), root.release(), cq, coll);
        }

        static const char* ns() { return "unittests.QueryPlanExecutor"; }

        size_t numCursors() {
            AutoGetCollectionForRead ctx(&_txn, ns() );
            Collection* collection = ctx.getCollection();
            if ( !collection )
                return 0;
            return collection->cursorCache()->numCursors();
        }

        void registerExec( PlanExecutor* exec ) {
            // TODO: This is not correct (create collection under S-lock)
            AutoGetCollectionForRead ctx(&_txn, ns());
            Collection* collection = ctx.getDb()->getOrCreateCollection(&_txn, ns());
            collection->cursorCache()->registerExecutor( exec );
        }

        void deregisterExec( PlanExecutor* exec ) {
            // TODO: This is not correct (create collection under S-lock)
            AutoGetCollectionForRead ctx(&_txn, ns());
            Collection* collection = ctx.getDb()->getOrCreateCollection(&_txn, ns());
            collection->cursorCache()->deregisterExecutor( exec );
        }

    protected:
        OperationContextImpl _txn;

    private:
        IndexDescriptor* getIndex(Database* db, const BSONObj& obj) {
            Collection* collection = db->getCollection( &_txn, ns() );
            return collection->getIndexCatalog()->findIndexByKeyPattern(&_txn, obj);
        }

        DBDirectClient _client;
    };

    /**
     * Test dropping the collection while the
     * PlanExecutor is doing a collection scan.
     */
    class DropCollScan : public PlanExecutorBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            insert(BSON("_id" << 1));
            insert(BSON("_id" << 2));

            BSONObj filterObj = fromjson("{_id: {$gt: 0}}");

            Collection* coll = ctx.getCollection();
            scoped_ptr<PlanExecutor> exec(makeCollScanExec(coll, filterObj));
            registerExec(exec.get());

            BSONObj objOut;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, NULL));
            ASSERT_EQUALS(1, objOut["_id"].numberInt());

            // After dropping the collection, the runner
            // should be dead.
            dropCollection();
            ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&objOut, NULL));

            deregisterExec(exec.get());
            ctx.commit();
        }
    };

    /**
     * Test dropping the collection while the PlanExecutor is doing an index scan.
     */
    class DropIndexScan : public PlanExecutorBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            insert(BSON("_id" << 1 << "a" << 6));
            insert(BSON("_id" << 2 << "a" << 7));
            insert(BSON("_id" << 3 << "a" << 8));
            BSONObj indexSpec = BSON("a" << 1);
            addIndex(indexSpec);

            scoped_ptr<PlanExecutor> exec(makeIndexScanExec(ctx.ctx(), indexSpec, 7, 10));
            registerExec(exec.get());

            BSONObj objOut;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, NULL));
            ASSERT_EQUALS(7, objOut["a"].numberInt());

            // After dropping the collection, the runner
            // should be dead.
            dropCollection();
            ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&objOut, NULL));

            deregisterExec(exec.get());
            ctx.commit();
        }
    };

    /**
     * Test dropping the collection while an agg PlanExecutor is doing an index scan.
     */
    class DropIndexScanAgg : public PlanExecutorBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());

            insert(BSON("_id" << 1 << "a" << 6));
            insert(BSON("_id" << 2 << "a" << 7));
            insert(BSON("_id" << 3 << "a" << 8));
            BSONObj indexSpec = BSON("a" << 1);
            addIndex(indexSpec);

            // Create the PlanExecutor which feeds the aggregation pipeline.
            boost::shared_ptr<PlanExecutor> innerExec(
                makeIndexScanExec(ctx.ctx(), indexSpec, 7, 10));

            // Create the aggregation pipeline.
            boost::intrusive_ptr<ExpressionContext> expCtx =
                new ExpressionContext(&_txn, NamespaceString(ns()));

            string errmsg;
            BSONObj inputBson = fromjson("{$match: {a: {$gte: 7, $lte: 10}}}");
            boost::intrusive_ptr<Pipeline> pipeline =
                Pipeline::parseCommand(errmsg, inputBson, expCtx);
            ASSERT_EQUALS(errmsg, "");

            // Create the output PlanExecutor that pulls results from the pipeline.
            std::auto_ptr<WorkingSet> ws(new WorkingSet());
            std::auto_ptr<PipelineProxyStage> proxy(
                new PipelineProxyStage(pipeline, innerExec, ws.get()));
            Collection* collection = ctx.getCollection();
            boost::scoped_ptr<PlanExecutor> outerExec(
                new PlanExecutor(&_txn, ws.release(), proxy.release(), collection));

            // Only the outer executor gets registered.
            registerExec(outerExec.get());

            // Verify that both the "inner" and "outer" plan executors have been killed after
            // dropping the collection.
            BSONObj objOut;
            dropCollection();
            ASSERT_EQUALS(PlanExecutor::DEAD, innerExec->getNext(&objOut, NULL));
            ASSERT_EQUALS(PlanExecutor::DEAD, outerExec->getNext(&objOut, NULL));

            deregisterExec(outerExec.get());
            ctx.commit();
        }
    };

    class SnapshotBase : public PlanExecutorBase {
    protected:
        void setupCollection() {
            insert(BSON("_id" << 1 << "a" << 1));
            insert(BSON("_id" << 2 << "a" << 2 << "payload" << "x"));
            insert(BSON("_id" << 3 << "a" << 3));
            insert(BSON("_id" << 4 << "a" << 4));
        }

        /**
         * Increases a document's size dramatically such that the document
         * exceeds the available padding and must be moved to the end of
         * the collection.
         */
        void forceDocumentMove() {
            BSONObj query = BSON("_id" << 2);
            BSONObj updateSpec = BSON("$set" << BSON("payload" << payload8k()));
            update(query, updateSpec);
        }

        std::string payload8k() {
            return std::string(8*1024, 'x');
        }

        /**
         * Given an array of ints, 'expectedIds', and a PlanExecutor,
         * 'exec', uses the executor to iterate through the collection. While
         * iterating, asserts that the _id of each successive document equals
         * the respective integer in 'expectedIds'.
         */
        void checkIds(int* expectedIds, PlanExecutor* exec) {
            BSONObj objOut;
            int idcount = 0;
            while (PlanExecutor::ADVANCED == exec->getNext(&objOut, NULL)) {
                ASSERT_EQUALS(expectedIds[idcount], objOut["_id"].numberInt());
                ++idcount;
            }
        }
    };

    /**
     * Create a scenario in which the same document is returned
     * twice due to a concurrent document move and collection
     * scan.
     */
    class SnapshotControl : public SnapshotBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            setupCollection();

            BSONObj filterObj = fromjson("{a: {$gte: 2}}");

            Collection* coll = ctx.getCollection();
            scoped_ptr<PlanExecutor> exec(makeCollScanExec(coll, filterObj));

            BSONObj objOut;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, NULL));
            ASSERT_EQUALS(2, objOut["a"].numberInt());

            forceDocumentMove();

            int ids[] = {3, 4, 2};
            checkIds(ids, exec.get());
            ctx.commit();
        }
    };

    /**
     * A snapshot is really just a hint that means scan the _id index.
     * Make sure that we do not see the document move with an _id
     * index scan.
     */
    class SnapshotTest : public SnapshotBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            setupCollection();
            BSONObj indexSpec = BSON("_id" << 1);
            addIndex(indexSpec);

            BSONObj filterObj = fromjson("{a: {$gte: 2}}");
            scoped_ptr<PlanExecutor> exec(makeIndexScanExec(ctx.ctx(), indexSpec, 2, 5));

            BSONObj objOut;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, NULL));
            ASSERT_EQUALS(2, objOut["a"].numberInt());

            forceDocumentMove();

            // Since this time we're scanning the _id index,
            // we should not see the moved document again.
            int ids[] = {3, 4};
            checkIds(ids, exec.get());
            ctx.commit();
        }
    };

    namespace ClientCursor {

        using mongo::ClientCursor;

        /**
         * Test invalidation of ClientCursor.
         */
        class Invalidate : public PlanExecutorBase {
        public:
            void run() {
                Client::WriteContext ctx(&_txn, ns());
                insert(BSON("a" << 1 << "b" << 1));

                BSONObj filterObj = fromjson("{_id: {$gt: 0}, b: {$gt: 0}}");

                Collection* coll = ctx.getCollection();
                PlanExecutor* exec = makeCollScanExec(coll,filterObj);

                // Make a client cursor from the runner.
                new ClientCursor(coll, exec, 0, BSONObj());

                // There should be one cursor before invalidation,
                // and zero cursors after invalidation.
                ASSERT_EQUALS(1U, numCursors());
                coll->cursorCache()->invalidateAll(false);
                ASSERT_EQUALS(0U, numCursors());
                ctx.commit();
            }
        };

        /**
         * Test that pinned client cursors persist even after
         * invalidation.
         */
        class InvalidatePinned : public PlanExecutorBase {
        public:
            void run() {
                Client::WriteContext ctx(&_txn, ns());
                insert(BSON("a" << 1 << "b" << 1));

                Collection* collection = ctx.getCollection();

                BSONObj filterObj = fromjson("{_id: {$gt: 0}, b: {$gt: 0}}");
                PlanExecutor* exec = makeCollScanExec(collection, filterObj);

                // Make a client cursor from the runner.
                ClientCursor* cc = new ClientCursor(collection, exec, 0, BSONObj());
                ClientCursorPin ccPin(collection, cc->cursorid());

                // If the cursor is pinned, it sticks around,
                // even after invalidation.
                ASSERT_EQUALS(1U, numCursors());
                collection->cursorCache()->invalidateAll(false);
                ASSERT_EQUALS(1U, numCursors());

                // The invalidation should have killed the runner.
                BSONObj objOut;
                ASSERT_EQUALS(PlanExecutor::DEAD, exec->getNext(&objOut, NULL));

                // Deleting the underlying cursor should cause the
                // number of cursors to return to 0.
                ccPin.deleteUnderlying();
                ASSERT_EQUALS(0U, numCursors());
                ctx.commit();
            }
        };

        /**
         * Test that client cursors time out and get
         * deleted.
         */
        class Timeout : public PlanExecutorBase {
        public:
            void run() {
                {
                    Client::WriteContext ctx(&_txn, ns());
                    insert(BSON("a" << 1 << "b" << 1));
                    ctx.commit();
                }

                {
                    AutoGetCollectionForRead ctx(&_txn, ns());
                    Collection* collection = ctx.getCollection();

                    BSONObj filterObj = fromjson("{_id: {$gt: 0}, b: {$gt: 0}}");
                    PlanExecutor* exec = makeCollScanExec(collection, filterObj);

                    // Make a client cursor from the runner.
                    new ClientCursor(collection, exec, 0, BSONObj());
                }

                // There should be one cursor before timeout,
                // and zero cursors after timeout.
                ASSERT_EQUALS(1U, numCursors());
                CollectionCursorCache::timeoutCursorsGlobal(&_txn, 600001);
                ASSERT_EQUALS(0U, numCursors());
            }
        };

    } // namespace ClientCursor

    class All : public Suite {
    public:
        All() : Suite( "query_plan_executor" ) { }

        void setupTests() {
            add<DropCollScan>();
            add<DropIndexScan>();
            add<DropIndexScanAgg>();
            add<SnapshotControl>();
            add<SnapshotTest>();
            add<ClientCursor::Invalidate>();
            add<ClientCursor::InvalidatePinned>();
            add<ClientCursor::Timeout>();
        }
    }  queryPlanExecutorAll;

}  // namespace QueryPlanExecutor

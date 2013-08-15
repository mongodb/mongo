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

#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryMultiPlanRunner {

    class MultiPlanRunnerBase {
    public:
        MultiPlanRunnerBase() { }

        virtual ~MultiPlanRunnerBase() {
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        IndexDescriptor* getIndex(const BSONObj& obj) {
            NamespaceDetails* nsd = nsdetails(ns());
            int idxNo = nsd->findIndexByKeyPattern(obj);
            return CatalogHack::getDescriptor(nsd, idxNo);
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        static const char* ns() { return "unittests.QueryStageMultiPlanRunner"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient MultiPlanRunnerBase::_client;

    // Basic ranking test: collection scan vs. highly selective index scan.  Make sure we also get
    // all expected results out as well.
    class MPRCollectionScanVsHighlySelectiveIXScan : public MultiPlanRunnerBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 5000;
            for (int i = 0; i < N; ++i) {
                insert(BSON("foo" << (i % 10)));
            }

            addIndex(BSON("foo" << 1));

            // Plan 0: IXScan over foo == 7
            // Every call to work() returns something so this should clearly win (by current scoring
            // at least).
            IndexScanParams ixparams;
            ixparams.descriptor = getIndex(BSON("foo" << 1));
            ixparams.bounds.isSimpleRange = true;
            ixparams.bounds.startKey = BSON("" << 7);
            ixparams.bounds.endKey = BSON("" << 7);
            ixparams.bounds.endKeyInclusive = true;
            ixparams.direction = 1;
            auto_ptr<WorkingSet> firstWs(new WorkingSet());
            IndexScan* ix = new IndexScan(ixparams, firstWs.get(), NULL);
            auto_ptr<PlanStage> firstRoot(new FetchStage(firstWs.get(), ix, NULL));

            // Plan 1: CollScan with matcher.
            CollectionScanParams csparams;
            csparams.ns = ns();
            csparams.direction = CollectionScanParams::FORWARD;
            auto_ptr<WorkingSet> secondWs(new WorkingSet());
            // Make the filter.
            BSONObj filterObj = BSON("foo" << 7);
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filter(swme.getValue());
            // Make the stage.
            auto_ptr<PlanStage> secondRoot(new CollectionScan(csparams, secondWs.get(),
                                                              filter.get()));

            // Hand the plans off to the runner.
            CanonicalQuery* cq = NULL;
            verify(CanonicalQuery::canonicalize(ns(), BSON("foo" << 7), &cq).isOK());
            verify(NULL != cq);
            MultiPlanRunner mpr(cq);
            mpr.addPlan(new QuerySolution(), firstRoot.release(), firstWs.release());
            mpr.addPlan(new QuerySolution(), secondRoot.release(), secondWs.release());

            // Plan 0 aka the first plan aka the index scan should be the best.
            size_t best;
            ASSERT(mpr.pickBestPlan(&best));
            ASSERT_EQUALS(size_t(0), best);

            // Get all our results out.
            int results = 0;
            BSONObj obj;
            while (Runner::RUNNER_ADVANCED == mpr.getNext(&obj, NULL)) {
                ASSERT_EQUALS(obj["foo"].numberInt(), 7);
                ++results;
            }

            ASSERT_EQUALS(results, N / 10);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_multi_plan_runner" ) { }

        void setupTests() {
            add<MPRCollectionScanVsHighlySelectiveIXScan>();
        }
    }  queryMultiPlanRunnerAll;

}  // namespace QueryMultiPlanRunner

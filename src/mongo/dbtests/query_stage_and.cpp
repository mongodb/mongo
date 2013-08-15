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
 * This file tests db/exec/and_*.cpp and DiskLoc invalidation.  DiskLoc invalidation forces a fetch
 * so we cannot test it outside of a dbtest.
 */

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageAnd {

    class QueryStageAndBase {
    public:
        QueryStageAndBase() { }

        virtual ~QueryStageAndBase() {
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

        void getLocs(set<DiskLoc>* out) {
            for (boost::shared_ptr<Cursor> c = theDataFileMgr.findAll(ns()); c->ok(); c->advance()) {
                out->insert(c->currLoc());
            }
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        int countResults(PlanStage* stage) {
            int count = 0;
            while (!stage->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState status = stage->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }
                ++count;
            }
            return count;
        }

        static const char* ns() { return "unittests.QueryStageAnd"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageAndBase::_client;

    //
    // Hash AND tests
    //

    /**
     * Invalidate a DiskLoc held by a hashed AND before the AND finishes evaluating.  The AND should
     * process all other data just fine and flag the invalidated DiskLoc in the WorkingSet.
     */
    class QueryStageAndHashInvalidation : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << i << "bar" << i));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndHashStage> ah(new AndHashStage(&ws, NULL));

            // Foo <= 20
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = -1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar >= 10
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 10);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // ah reads the first child into its hash table.
            // ah should read foo=20, foo=19, ..., foo=0 in that order.
            // Read half of them...
            for (int i = 0; i < 10; ++i) {
                WorkingSetID out;
                PlanStage::StageState status = ah->work(&out);
                ASSERT_EQUALS(PlanStage::NEED_TIME, status);
            }

            // ...yield
            ah->prepareToYield();
            // ...invalidate one of the read objects
            set<DiskLoc> data;
            getLocs(&data);
            for (set<DiskLoc>::const_iterator it = data.begin(); it != data.end(); ++it) {
                if (it->obj()["foo"].numberInt() == 15) {
                    ah->invalidate(*it);
                    remove(it->obj());
                    break;
                }
            }
            ah->recoverFromYield();

            // And expect to find foo==15 it flagged for review.
            const vector<WorkingSetID>& flagged = ws.getFlagged();
            ASSERT_EQUALS(size_t(1), flagged.size());

            // Expect to find the right value of foo in the flagged item.
            WorkingSetMember* member = ws.get(flagged[0]);
            ASSERT_TRUE(NULL != member);
            ASSERT_EQUALS(WorkingSetMember::OWNED_OBJ, member->state);
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(15, elt.numberInt());

            // Now, finish up the AND.  Since foo == bar, we would have 11 results, but we subtract
            // one because of a mid-plan invalidation, so 10.
            int count = 0;
            while (!ah->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState status = ah->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }

                ++count;
                member = ws.get(id);

                ASSERT_TRUE(member->getFieldDotted("foo", &elt));
                ASSERT_LESS_THAN_OR_EQUALS(elt.numberInt(), 20);
                ASSERT_NOT_EQUALS(15, elt.numberInt());
                ASSERT_TRUE(member->getFieldDotted("bar", &elt));
                ASSERT_GREATER_THAN_OR_EQUALS(elt.numberInt(), 10);
            }

            ASSERT_EQUALS(10, count);
        }
    };


    // An AND with three children.
    class QueryStageAndHashThreeLeaf : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << i << "bar" << i << "baz" << i));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));
            addIndex(BSON("baz" << 1));

            WorkingSet ws;
            scoped_ptr<AndHashStage> ah(new AndHashStage(&ws, NULL));

            // Foo <= 20
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = -1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar >= 10
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 10);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // 5 <= baz <= 15
            params.descriptor = getIndex(BSON("baz" << 1));
            params.bounds.startKey = BSON("" << 5);
            params.bounds.endKey = BSON("" << 15);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // foo == bar == baz, and foo<=20, bar>=10, 5<=baz<=15, so our values are:
            // foo == 10, 11, 12, 13, 14, 15.
            ASSERT_EQUALS(6, countResults(ah.get()));
        }
    };

    // An AND with an index scan that returns nothing.
    class QueryStageAndHashWithNothing : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << i << "bar" << 20));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndHashStage> ah(new AndHashStage(&ws, NULL));

            // Foo <= 20
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = -1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar == 5.  Index scan should be eof.
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 5);
            params.bounds.endKey = BSON("" << 5);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            ASSERT_EQUALS(0, countResults(ah.get()));
        }
    };

    // An AND that scans data but returns nothing.
    class QueryStageAndHashProducesNothing : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 10; ++i) {
                insert(BSON("foo" << (100 + i)));
                insert(BSON("bar" << i));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndHashStage> ah(new AndHashStage(&ws, NULL));

            // Foo >= 100
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 100);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar <= 100
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 100);
            // This is subtle and confusing.  We couldn't extract any keys from the elements with
            // 'foo' in them so we would normally index them with the "nothing found" key.  We don't
            // want to include that in our scan.
            params.bounds.endKey = BSON("" << "");
            params.bounds.endKeyInclusive = false;
            params.direction = -1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            ASSERT_EQUALS(0, countResults(ah.get()));
        }
    };

    // An AND that would return more data but the matcher filters it.
    class QueryStageAndHashWithMatcher : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << i << "bar" << (100 - i)));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            BSONObj filter = BSON("bar" << 97);
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filter);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());
            scoped_ptr<AndHashStage> ah(new AndHashStage(&ws, filterExpr.get()));

            // Foo <= 20
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = -1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar >= 95
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 10);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar == 97
            ASSERT_EQUALS(1, countResults(ah.get()));
        }
    };

    //
    // Sorted AND tests
    //

    /**
     * Invalidate a DiskLoc held by a sorted AND before the AND finishes evaluating.  The AND should
     * process all other data just fine and flag the invalidated DiskLoc in the WorkingSet.
     */
    class QueryStageAndSortedInvalidation : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert a bunch of data
            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << 1 << "bar" << 1));
            }
            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndSortedStage> ah(new AndSortedStage(&ws, NULL));

            // Scan over foo == 1
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 1);
            params.bounds.endKey = BSON("" << 1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Scan over bar == 1
            params.descriptor = getIndex(BSON("bar" << 1));
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Get the set of disklocs in our collection to use later.
            set<DiskLoc> data;
            getLocs(&data);

            // We're making an assumption here that happens to be true because we clear out the
            // collection before running this: increasing inserts have increasing DiskLocs.
            // This isn't true in general if the collection is not dropped beforehand.
            WorkingSetID id;

            // Sorted AND looks at the first child, which is an index scan over foo==1.
            ah->work(&id);

            // The first thing that the index scan returns (due to increasing DiskLoc trick) is the
            // very first insert, which should be the very first thing in data.  Let's invalidate it
            // and make sure it shows up in the flagged results.
            ah->prepareToYield();
            ah->invalidate(*data.begin());
            remove(data.begin()->obj());
            ah->recoverFromYield();

            // Make sure the nuked obj is actually in the flagged data.
            ASSERT_EQUALS(ws.getFlagged().size(), size_t(1));
            WorkingSetMember* member = ws.get(ws.getFlagged()[0]);
            ASSERT_EQUALS(WorkingSetMember::OWNED_OBJ, member->state);
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_EQUALS(1, elt.numberInt());

            set<DiskLoc>::iterator it = data.begin();

            // Proceed along, AND-ing results.
            int count = 0;
            while (!ah->isEOF() && count < 10) {
                WorkingSetID id;
                PlanStage::StageState status = ah->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }

                ++count;
                ++it;
                member = ws.get(id);

                ASSERT_TRUE(member->getFieldDotted("foo", &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT_TRUE(member->getFieldDotted("bar", &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT_EQUALS(member->loc, *it);
            }

            // Move 'it' to a result that's yet to show up.
            for (int i = 0; i < count + 10; ++i) { ++it; }
            // Remove a result that's coming up.  It's not the 'target' result of the AND so it's
            // not flagged.
            ah->prepareToYield();
            ah->invalidate(*it);
            remove(it->obj());
            ah->recoverFromYield();

            // Get all results aside from the two we killed.
            while (!ah->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState status = ah->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }

                ++count;
                member = ws.get(id);

                ASSERT_TRUE(member->getFieldDotted("foo", &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT_TRUE(member->getFieldDotted("bar", &elt));
                ASSERT_EQUALS(1, elt.numberInt());
            }

            ASSERT_EQUALS(count, 48);

            ASSERT_EQUALS(size_t(1), ws.getFlagged().size());
        }
    };


    // An AND with three children.
    class QueryStageAndSortedThreeLeaf : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

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

            WorkingSet ws;
            scoped_ptr<AndSortedStage> ah(new AndSortedStage(&ws, NULL));

            // Scan over foo == 1
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 1);
            params.bounds.endKey = BSON("" << 1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // bar == 1
            params.descriptor = getIndex(BSON("bar" << 1));
            ah->addChild(new IndexScan(params, &ws, NULL));

            // baz == 1
            params.descriptor = getIndex(BSON("baz" << 1));
            ah->addChild(new IndexScan(params, &ws, NULL));

            ASSERT_EQUALS(50, countResults(ah.get()));
        }
    };

    // An AND with an index scan that returns nothing.
    class QueryStageAndSortedWithNothing : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << 8 << "bar" << 20));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndSortedStage> ah(new AndSortedStage(&ws, NULL));

            // Foo == 7.  Should be EOF.
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 7);
            params.bounds.endKey = BSON("" << 7);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Bar == 20, not EOF.
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 20);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            ASSERT_EQUALS(0, countResults(ah.get()));
        }
    };

    // An AND that scans data but returns nothing.
    class QueryStageAndSortedProducesNothing : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                // Insert data with foo=7, bar==20, but nothing with both.
                insert(BSON("foo" << 8 << "bar" << 20));
                insert(BSON("foo" << 7 << "bar" << 21));
                insert(BSON("foo" << 7));
                insert(BSON("bar" << 20));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            scoped_ptr<AndSortedStage> ah(new AndSortedStage(&ws, NULL));

            // foo == 7.
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 7);
            params.bounds.endKey = BSON("" << 7);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // bar == 20.
            params.descriptor = getIndex(BSON("bar" << 1));
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 20);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            ASSERT_EQUALS(0, countResults(ah.get()));
        }
    };

    // An AND that would return data but the matcher prevents it.
    class QueryStageAndSortedWithMatcher : public QueryStageAndBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < 50; ++i) {
                insert(BSON("foo" << 1 << "bar" << 1));
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("bar" << 1));

            WorkingSet ws;
            BSONObj filterObj = BSON("foo" << BSON("$ne" << 1));
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());
            scoped_ptr<AndSortedStage> ah(new AndSortedStage(&ws, filterExpr.get()));

            // Scan over foo == 1
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 1);
            params.bounds.endKey = BSON("" << 1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ah->addChild(new IndexScan(params, &ws, NULL));

            // bar == 1
            params.descriptor = getIndex(BSON("bar" << 1));
            ah->addChild(new IndexScan(params, &ws, NULL));

            // Filter drops everything.
            ASSERT_EQUALS(0, countResults(ah.get()));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_and" ) { }

        void setupTests() {
            add<QueryStageAndHashInvalidation>();
            add<QueryStageAndHashThreeLeaf>();
            add<QueryStageAndHashWithNothing>();
            add<QueryStageAndHashProducesNothing>();
            add<QueryStageAndHashWithMatcher>();
            add<QueryStageAndSortedInvalidation>();
            add<QueryStageAndSortedThreeLeaf>();
            add<QueryStageAndSortedWithNothing>();
            add<QueryStageAndSortedProducesNothing>();
            add<QueryStageAndSortedWithMatcher>();
        }
    }  queryStageAndAll;

}  // namespace QueryStageAnd

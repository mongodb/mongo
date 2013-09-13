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

#include "mongo/db/query/plan_enumerator.h"

#include <boost/scoped_ptr.hpp>
#include <set>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/predicate_map.h"
#include "mongo/unittest/unittest.h"

namespace {

    using boost::scoped_ptr;
    using mongo::BSONObj;
    using mongo::fromjson;
    using mongo::IndexTag;
    using mongo::MatchExpression;
    using mongo::MatchExpressionParser;
    using mongo::PlanEnumerator;
    using mongo::PredicateInfo;
    using mongo::PredicateMap;
    using mongo::RelevantIndex;
    using mongo::StatusWithMatchExpression;
    using std::set;
    using std::vector;

    /**
     * XXX Test plan
     *
     * We'd like to test how the enumerator behaves when it picks one index at a time. We'll
     * start the progression of tests going over increasingly complex queries in which that
     * index can apply (sometimes more than onece).  We check whether the enumerator produces
     * as many plans as we expected and whether the annotations (IndexTag's) were done
     * correctly.
     *
     * Tricky part here is how to make this test useful in the long term. It is likely that
     * we'll change the order in which plans are produced (e.g., try to get to the good
     * plans faster) and we wouldn't like to destabilize the tests in here as we do so.
     *
     * XXX
     */

    TEST(SingleIndex, OnePredicate) {
        BSONObj filter = fromjson("{a:99}");
        StatusWithMatchExpression swme = MatchExpressionParser::parse(filter);
        ASSERT_TRUE(swme.isOK());
        MatchExpression* root = swme.getValue();

        // Build a predicate map where an index {a:1} is relevant for the node 'a' EQ 99.
        vector<BSONObj> indexKeyPatterns;
        indexKeyPatterns.push_back(fromjson("{a:1}"));

        scoped_ptr<PredicateMap> pm (new PredicateMap);
        PredicateInfo pred(root);
        std::set<RelevantIndex>& relevantIndices = pred.relevant;
        RelevantIndex ri(0, RelevantIndex::FIRST);
        relevantIndices.insert(ri);
        pm->insert(std::make_pair("a", pred));

        // Check that the enumerator takes a single predicate predicate map.
        PlanEnumerator enumerator(root, pm.get(), &indexKeyPatterns);
        ASSERT_OK(enumerator.init());

        // The only possible plan is one where the relevant index is used to the equality node.
        MatchExpression* annotated;
        ASSERT_TRUE(enumerator.getNext(&annotated));
        IndexTag* indexTag = static_cast<IndexTag*>(annotated->getTag());
        ASSERT_NOT_EQUALS(indexTag, static_cast<IndexTag*>(NULL));
        ASSERT_EQUALS(indexTag->index, 0U);
        delete annotated;

        // There's no other possible plan.
        ASSERT_FALSE(enumerator.getNext(&annotated));
    }

} // unnamed namespace

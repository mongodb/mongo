// expression_algo_test.cpp

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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/unittest/unittest.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#include "mongo/util/log.h"

namespace mongo {

    using boost::scoped_ptr;

    /**
     * A MatchExpression does not hold the memory for BSONElements.
     * So using this we can tie the life cycle of a MatchExpression to its data.
     */
    struct Parsed {
        Parsed(const char* str) {
            obj = fromjson(str);
            _parse();
        }

        Parsed(const BSONObj& o)
            : obj(o) {
            _parse();
        }

        void _parse() {
            StatusWithMatchExpression result = MatchExpressionParser::parse(obj);
            if (!result.isOK()) {
                log() << "failed to parse expression: " << obj;
                invariant(false);
            }
            exp.reset(result.getValue());
        }

        const MatchExpression* get() const { return exp.get(); }

        BSONObj obj;
        scoped_ptr<MatchExpression> exp;
    };

    TEST(ExpressionAlgoRedundant, Equal1) {
        Parsed foo("{ x : 5 }");
        Parsed bar1("{ x : 5 }");
        Parsed bar2("{ x : 6 }");
        Parsed bar3("{ a : 5 }");
        ASSERT_TRUE(expression::isClauseRedundant(foo.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar3.get()));
    }

    TEST(ExpressionAlgoRedundant, AndEqual1) {
        Parsed foo("{ a : 3, x : 5 }");
        Parsed bar1("{ x : 5 }");
        Parsed bar2("{ x : 6 }");
        Parsed bar3("{ a : 5 }");
        ASSERT_TRUE(expression::isClauseRedundant(foo.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar3.get()));
        ASSERT_FALSE(expression::isClauseRedundant(bar1.get(), foo.get()));
    }

    TEST(ExpressionAlgoRedundant, DifferentTypes1) {
        // Comparison of different canonical types is not redundant.
        Parsed foo("{x: {$gt: \"a\"}}");
        Parsed bar("{x: {$gt: 1}}");
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar.get()));
        ASSERT_FALSE(expression::isClauseRedundant(bar.get(), foo.get()));
    }

    TEST(ExpressionAlgoRedundant, DifferentTypes2) {
        Parsed foo("{x: null}");
        Parsed bar("{x: {$exists: true}}");
        ASSERT_FALSE(expression::isClauseRedundant(foo.get(), bar.get()));
    }

    TEST(ExpressionAlgoRedundant, PointToRange) {
        Parsed foo1("{ x : 4 }");
        Parsed foo2("{ x : 5 }");
        Parsed foo3("{ a : 4 }");
        Parsed foo4("{ x : 6 }");
        Parsed foo5("{ a : 6 }");

        Parsed bar1("{ x : { $lte : 5 } }");
        Parsed bar2("{ x : { $lt : 5 } }");
        Parsed bar3("{ x : { $gte : 5 } }");
        Parsed bar4("{ x : { $gt : 5 } }");

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar1.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(bar1.get(), foo1.get()));

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo2.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar2.get()));

        ASSERT_FALSE(expression::isClauseRedundant(foo1.get(), bar3.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar3.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar3.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo4.get(), bar3.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar3.get()));

        ASSERT_FALSE(expression::isClauseRedundant(foo1.get(), bar4.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo2.get(), bar4.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar4.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo4.get(), bar4.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar4.get()));
    }

    TEST(ExpressionAlgoRedundant, LessThanToLessThan) {
        Parsed foo1("{ x : { $lte : 4 } }");
        Parsed foo2("{ x : { $lt : 5 } }");
        Parsed foo3("{ x : { $lte : 5 } }");
        Parsed foo4("{ x : { $lte : 6 } }");
        Parsed foo5("{ a : { $lte : 4 } }");

        Parsed bar1("{ x : { $lte : 5 } }");
        Parsed bar2("{ x : { $lt : 5 } }");

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar1.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar1.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo3.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar1.get()));

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar2.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar2.get()));

    }

    TEST(ExpressionAlgoRedundant, GreaterThanToGreaterThan) {
        Parsed foo1("{ x : { $gte : 6 } }");
        Parsed foo2("{ x : { $gt : 5 } }");
        Parsed foo3("{ x : { $gte : 5 } }");
        Parsed foo4("{ x : { $gte : 4 } }");
        Parsed foo5("{ a : { $gte : 6 } }");

        Parsed bar1("{ x : { $gte : 5 } }");
        Parsed bar2("{ x : { $gt : 5 } }");

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar1.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar1.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo3.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar1.get()));

        ASSERT_TRUE(expression::isClauseRedundant(foo1.get(), bar2.get()));
        ASSERT_TRUE(expression::isClauseRedundant(foo2.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo3.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo4.get(), bar2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(foo5.get(), bar2.get()));

    }

    TEST(ExpressionAlgoRedundant, Exists1) {
        Parsed a("{a : { $exists : 1 } }");
        Parsed b("{b : { $exists : 1 } }");
        Parsed ab("{a : { $exists : 1 }, b : { $exists: 1 } }");
        Parsed abc("{a : { $exists : 1 }, b : { $exists: 1 }, c : 5 }");

        ASSERT_TRUE(expression::isClauseRedundant(a.get(), a.get()));
        ASSERT_FALSE(expression::isClauseRedundant(a.get(), b.get()));
        ASSERT_FALSE(expression::isClauseRedundant(b.get(), a.get()));

        ASSERT_TRUE(expression::isClauseRedundant(ab.get(), a.get()));
        ASSERT_TRUE(expression::isClauseRedundant(ab.get(), b.get()));

        ASSERT_TRUE(expression::isClauseRedundant(abc.get(), a.get()));
        ASSERT_TRUE(expression::isClauseRedundant(abc.get(), b.get()));

        ASSERT_TRUE(expression::isClauseRedundant(abc.get(), ab.get()));
        ASSERT_FALSE(expression::isClauseRedundant(ab.get(), abc.get()));
    }

    TEST(ExpressionAlgoRedundant, Exists2) {
        Parsed filter("{a : { $exists : 1 } }");
        Parsed query1("{a : 1}");
        Parsed query2("{a : { $gt : 4 } }");
        Parsed query3("{a : { $lt : 4 } }");

        ASSERT_TRUE(expression::isClauseRedundant(query1.get(), filter.get()));
        ASSERT_TRUE(expression::isClauseRedundant(query2.get(), filter.get()));
        ASSERT_TRUE(expression::isClauseRedundant(query3.get(), filter.get()));
        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query1.get()));
    }

    TEST(ExpressionAlgoRedundant, Exists3) {
        Parsed filter("{a : { $exists : 1 } }");
        Parsed query1("{a : { $type : 5 } }");

        ASSERT_TRUE(expression::isClauseRedundant(query1.get(), filter.get()));
        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query1.get()));
    }

    TEST(ExpressionAlgoRedundant, Exists4) {
        Parsed filter("{a : { $exists : 1 } }");
        Parsed query1("{b : { $type : 5 } }");

        ASSERT_FALSE(expression::isClauseRedundant(query1.get(), filter.get()));
        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query1.get()));
    }

    TEST(ExpressionAlgoRedundant, Type1) {
        Parsed a("{a : { $type : 4 } }");
        Parsed a2("{a : { $type : 4 } }");
        Parsed b("{a : { $type : 7 } }");

        ASSERT_TRUE(expression::isClauseRedundant(a.get(), a2.get()));
        ASSERT_FALSE(expression::isClauseRedundant(a.get(), b.get()));
    }

    TEST(ExpressionAlgoRedundant, Subset1) {
        Parsed filter("{ a : 5, b : 6 }");
        Parsed query("{ a : 5, b : 6, c : 7 }");

        ASSERT_TRUE(expression::isClauseRedundant(query.get(), filter.get()));
        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query.get()));
    }

    TEST(ExpressionAlgoRedundant, Subset2) {
        Parsed filter("{ a : { $gt : 5 }, b : { $gt : 6 } }");
        Parsed query1("{ a : { $gt : 5 }, b : { $gt : 6 }, c : { $gt : 7 } }");
        Parsed query2("{ a : 10, b : 10, c : 10 }");

        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query1.get()));
        ASSERT_FALSE(expression::isClauseRedundant(filter.get(), query2.get()));

        ASSERT_TRUE(expression::isClauseRedundant(query1.get(), filter.get()));
        ASSERT_TRUE(expression::isClauseRedundant(query2.get(), filter.get()));

    }

}


// expression_algo.cpp

/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/util/log.h"

//#define DDD(x) log() << x
#define DDD(x)

namespace mongo {
    namespace {

        bool _pathMatches(const LeafMatchExpression* left,
                          const MatchExpression* bar) {
            invariant(left);
            invariant(bar);
            const LeafMatchExpression* right = dynamic_cast<const LeafMatchExpression*>(bar);
            if (!right)
                return false;

            return left->path() == right->path();
        }

        bool _typeAndPathCompatable(const ComparisonMatchExpression* left,
                                    const ComparisonMatchExpression* right) {
            if (!_pathMatches(left, right))
                return false;

            if (left->getData().canonicalType() != right->getData().canonicalType())
                return false;

            return true;
        }


        bool _isRedundantEQHelp(const ComparisonMatchExpression* left,
                                const MatchExpression* bar,
                                bool isLessThan,
                                bool equalOk) {
            const ComparisonMatchExpression* right =
                dynamic_cast<const ComparisonMatchExpression*>(bar);
            invariant(right);

            if (!_typeAndPathCompatable(left, right))
                return false;

            int cmp = left->getData().woCompare(right->getData(), false);
            if (isLessThan) {
                if (cmp < 0)
                    return true;
                if (cmp == 0)
                    return equalOk;
                return false;
            }

            if (cmp > 0)
                return true;
            if (cmp == 0)
                return equalOk;
            return false;
        }

        /**
         * @param foo is a literal something that has to match exactly
         * @return if the expression bar guarantees returning any document matching foo
         */
        bool isRedundantEQ(const MatchExpression* foo,
                           const MatchExpression* bar) {
            const ComparisonMatchExpression* equal =
                dynamic_cast<const ComparisonMatchExpression*>(foo);
            invariant(equal);

            DDD("isRedundantEQ");

            switch(bar->matchType()) {
            case MatchExpression::EQ:
                // would be handled elsewhere
                return false;

            case MatchExpression::LT:
                return _isRedundantEQHelp(equal, bar, true, false);
            case MatchExpression::LTE:
                return _isRedundantEQHelp(equal, bar, true, true);

            case MatchExpression::GT:
                return _isRedundantEQHelp(equal, bar, false, false);
            case MatchExpression::GTE:
                return _isRedundantEQHelp(equal, bar, false, true);

            case MatchExpression::EXISTS: {
                switch (equal->getData().type()) {
                case jstNULL:
                case Undefined:
                case EOO:
                    return false;
                default:
                    return _pathMatches(equal, bar);
                }
            }

            default:
                return false;
            }
        }

        bool isRedundantLT(const MatchExpression* foo,
                           const MatchExpression* bar,
                           bool equalOk) {
            const ComparisonMatchExpression* left =
                dynamic_cast<const ComparisonMatchExpression*>(foo);
            invariant(left);


            if(bar->matchType() == MatchExpression::LT ||
               bar->matchType() == MatchExpression::LTE ) {

                const ComparisonMatchExpression* right =
                    dynamic_cast<const ComparisonMatchExpression*>(bar);
                invariant(right);

                if (!_typeAndPathCompatable(left, right))
                    return false;

                int cmp = left->getData().woCompare(right->getData(), false);
                if (cmp == 0) {
                    if(bar->matchType() == MatchExpression::LTE)
                        return true;
                    if(!equalOk)
                        return true;
                    return false;
                }
                return cmp < 0;
            }
            else if(bar->matchType() == MatchExpression::EXISTS) {
                return _pathMatches(left, bar);
            }

            return false;
        }

        bool isRedundantGT(const MatchExpression* foo,
                           const MatchExpression* bar,
                           bool equalOk) {
            const ComparisonMatchExpression* left =
                dynamic_cast<const ComparisonMatchExpression*>(foo);
            invariant(left);


            if(bar->matchType() == MatchExpression::GT ||
               bar->matchType() == MatchExpression::GTE ) {

                const ComparisonMatchExpression* right =
                    dynamic_cast<const ComparisonMatchExpression*>(bar);
                invariant(right);

                if (!_typeAndPathCompatable(left, right))
                    return false;

                int cmp = left->getData().woCompare(right->getData(), false);
                if (cmp == 0) {
                    if(bar->matchType() == MatchExpression::GTE)
                        return true;
                    if(!equalOk)
                        return true;
                    return false;
                }
                return cmp > 0;
            }
            else if(bar->matchType() == MatchExpression::EXISTS) {
                return _pathMatches(left, bar);
            }

            return false;
        }

    }



    namespace expression {

        bool isClauseRedundant(const MatchExpression* foo,
                               const MatchExpression* bar) {

            if (bar == NULL ||
                foo == bar) {
                return true;
            }
            if (foo == NULL) {
                return false;
            }


            DDD("isClauseRedundant\n"
                << "foo: " << foo->toString()
                << "bar: " << bar->toString());

            if (foo->equivalent(bar)) {
                DDD("t equivalent!");
                return true;
            }

            switch(foo->matchType()) {
            case MatchExpression::AND: {
                for (size_t i = 0; i < foo->numChildren(); i++ ) {
                    if(isClauseRedundant(foo->getChild(i), bar)) {
                        return true;
                    }
                }

                if (bar->matchType() == MatchExpression::AND) {
                    // everything in bar has to appear in foo
                    for (size_t i = 0; i < bar->numChildren(); i++ ) {
                        if(!isClauseRedundant(foo, bar->getChild(i))) {
                            return false;
                        }
                    }
                    return true;
                }

                return false;
            }
            case MatchExpression::OR: {
                // TODO: $or each clause has to be redundant
                return false;
            }

            case MatchExpression::EQ:
                return isRedundantEQ(foo, bar);

            case MatchExpression::LT:
                return isRedundantLT(foo, bar, false);
            case MatchExpression::LTE:
                return isRedundantLT(foo, bar, true);

            case MatchExpression::GT:
                return isRedundantGT(foo, bar, false);
            case MatchExpression::GTE:
                return isRedundantGT(foo, bar, true);

            case MatchExpression::TYPE_OPERATOR: {
                if (bar->matchType() != MatchExpression::EXISTS) {
                    return false;
                }

                const TypeMatchExpression* a = dynamic_cast<const TypeMatchExpression*>(foo);
                const ExistsMatchExpression* b = dynamic_cast<const ExistsMatchExpression*>(bar);

                return a->path() == b->path();
            }

            default:
                return false;
            }

            return false;
        }
    }
}

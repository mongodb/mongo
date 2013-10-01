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

#include "mongo/db/matcher/expression.h"

#pragma once

namespace mongo {

    /**
     * Logic for how indices can be used with an expression.
     */
    class Indexability {
    public:
        /**
         * Is an index over me->path() useful?
         * This is the same thing as being sargable, if you have a RDBMS background.
         */
        static bool nodeCanUseIndexOnOwnField(const MatchExpression* me) {
            if (me->path().empty()) {
                return false;
            }

            if (arrayUsesIndexOnOwnField(me)) {
                return true;
            }

            return    me->matchType() == MatchExpression::LTE
                   || me->matchType() == MatchExpression::LT
                   || me->matchType() == MatchExpression::EQ
                   || me->matchType() == MatchExpression::GT
                   || me->matchType() == MatchExpression::GTE
                   || me->matchType() == MatchExpression::REGEX
                   || me->matchType() == MatchExpression::MOD
                   || me->matchType() == MatchExpression::MATCH_IN
                   || me->matchType() == MatchExpression::TYPE_OPERATOR
                   || me->matchType() == MatchExpression::GEO
                   || me->matchType() == MatchExpression::GEO_NEAR;
        }

        /**
         * This array operator doesn't have any children with fields and can use an index.
         *
         * Example: a: {$elemMatch: {$gte: 1, $lte: 1}}.
         */
        static bool arrayUsesIndexOnOwnField(const MatchExpression* me) {
            return me->isArray() && MatchExpression::ELEM_MATCH_VALUE == me->matchType();
        }

        /**
         * Certain array operators require that the field for that operator is prepended
         * to all fields in that operator's children.
         *
         * Example: a: {$elemMatch: {b:1, c:1}}.
         */
        static bool arrayUsesIndexOnChildren(const MatchExpression* me) {
            return me->isArray() && (MatchExpression::ELEM_MATCH_OBJECT == me->matchType()
                                     || MatchExpression::ALL == me->matchType());
        };
    };

}  // namespace mongo

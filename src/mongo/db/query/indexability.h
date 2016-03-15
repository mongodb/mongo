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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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

        return isIndexOnOwnFieldTypeNode(me);
    }

    /**
     * This array operator doesn't have any children with fields and can use an index.
     *
     * Example: a: {$elemMatch: {$gte: 1, $lte: 1}}.
     */
    static bool arrayUsesIndexOnOwnField(const MatchExpression* me) {
        if (!me->isArray()) {
            return false;
        }

        if (MatchExpression::ELEM_MATCH_VALUE != me->matchType()) {
            return false;
        }

        // We have an ELEM_MATCH_VALUE expression. In order to be
        // considered "indexable" all children of the ELEM_MATCH_VALUE
        // must be "indexable" type expressions as well.
        for (size_t i = 0; i < me->numChildren(); i++) {
            MatchExpression* child = me->getChild(i);

            // Special case for NOT: If the child is a NOT, then it's the thing below
            // the NOT that we care about.
            if (MatchExpression::NOT == child->matchType()) {
                MatchExpression* notChild = child->getChild(0);

                if (MatchExpression::MOD == notChild->matchType() ||
                    MatchExpression::REGEX == notChild->matchType() ||
                    MatchExpression::TYPE_OPERATOR == notChild->matchType()) {
                    // We can't index negations of this kind of expression node.
                    return false;
                }

                // It's the child of the NOT that we check for indexability.
                if (!isIndexOnOwnFieldTypeNode(notChild)) {
                    return false;
                }

                // Special handling for NOT has already been done; don't fall through.
                continue;
            }

            if (!isIndexOnOwnFieldTypeNode(child)) {
                return false;
            }
        }

        // The entire ELEM_MATCH_VALUE is indexable since every one of its children
        // is indexable.
        return true;
    }

    /**
     * Certain array operators require that the field for that operator is prepended
     * to all fields in that operator's children.
     *
     * Example: a: {$elemMatch: {b:1, c:1}}.
     */
    static bool arrayUsesIndexOnChildren(const MatchExpression* me) {
        return me->isArray() && MatchExpression::ELEM_MATCH_OBJECT == me->matchType();
    }

    /**
     * Returns true if 'me' is a NOT, and the child of the NOT can use
     * an index on its own field.
     */
    static bool isBoundsGeneratingNot(const MatchExpression* me) {
        return MatchExpression::NOT == me->matchType() &&
            nodeCanUseIndexOnOwnField(me->getChild(0));
    }

    /**
     * Returns true if either 'me' is a bounds generating NOT,
     * or 'me' can use an index on its own field.
     */
    static bool isBoundsGenerating(const MatchExpression* me) {
        return isBoundsGeneratingNot(me) || nodeCanUseIndexOnOwnField(me);
    }

    /**
     * Returns true if 'me' is of type EQ, GT, GTE, LT, or LTE.
     */
    static bool isEqualityOrInequality(const MatchExpression* me) {
        return (me->matchType() == MatchExpression::EQ || me->matchType() == MatchExpression::GT ||
                me->matchType() == MatchExpression::GTE || me->matchType() == MatchExpression::LT ||
                me->matchType() == MatchExpression::LTE);
    }

private:
    /**
     * Returns true if 'me' is "sargable" but is not a negation and
     * is not an array node such as ELEM_MATCH_VALUE.
     *
     * Used as a helper for nodeCanUseIndexOnOwnField().
     */
    static bool isIndexOnOwnFieldTypeNode(const MatchExpression* me) {
        return me->matchType() == MatchExpression::LTE || me->matchType() == MatchExpression::LT ||
            me->matchType() == MatchExpression::EQ || me->matchType() == MatchExpression::GT ||
            me->matchType() == MatchExpression::GTE || me->matchType() == MatchExpression::REGEX ||
            me->matchType() == MatchExpression::MOD ||
            me->matchType() == MatchExpression::MATCH_IN ||
            me->matchType() == MatchExpression::TYPE_OPERATOR ||
            me->matchType() == MatchExpression::GEO ||
            me->matchType() == MatchExpression::GEO_NEAR ||
            me->matchType() == MatchExpression::EXISTS || me->matchType() == MatchExpression::TEXT;
    }
};

}  // namespace mongo

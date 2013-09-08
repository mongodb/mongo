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

#pragma once

#include <set>
#include <vector>

#include "mongo/db/matcher/expression.h"

namespace mongo {

    /**
     * Describes an index that could be used to answer a predicate.
     */
    struct RelevantIndex {

        enum Relevance {
            // Is the index prefixed by the predicate's field?  If so it can be used.
            FIRST,

            // Is the predicate's field in the index but not as a prefix?  If so, the index might be
            // able to be used, if there is another predicate that prefixes the index.
            NOT_FIRST,
        };

        RelevantIndex(int i, Relevance r) : index(i), relevance(r) { }

        // To allow insertion into a set and sorting by something.
        bool operator<(const RelevantIndex& other) const {
            // We're only ever comparing these inside of a predicate.  A predicate should only be
            // tagged for an index once.  This of course assumes that values are only indexed once
            // in an index.
            verify(other.index != index);
            return index < other.index;
        }

        // What index is relevant?
        int index;

        // How useful is it?
        Relevance relevance;
    };

    /**
     * Caches information about the predicates we're trying to plan for.
     */
    struct PredicateInfo {

        PredicateInfo(MatchExpression* node) {
            nodes.push_back(node);
        }

        // Any relevant indices.  Ordered by index no.
        set<RelevantIndex> relevant;

        // Which nodes is this expression 'path matchType *' found in?  Not owned here.
        vector<MatchExpression*> nodes;
    };

    // This is a multimap because the same field name can easily appear more than once in a query.
    typedef map<string, PredicateInfo> PredicateMap;

} // namespace mongo

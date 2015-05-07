/**
 *    Copyright (C) 2015 10gen Inc.
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

#pragma once

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/string_map.h"

namespace mongo {

    class BSONObj;
    class MatchExpression;
    struct IndexEntry;

    using IndexabilityDiscriminator = stdx::function<bool(const MatchExpression* me)>;
    using IndexabilityDiscriminators = std::vector<IndexabilityDiscriminator>;

    /**
     * PlanCacheIndexabilityState holds a set of "indexability discriminators" for certain paths.
     * An indexability discriminator is a binary predicate function, used to classify match
     * expressions based on the data values in the expression.
     */
    class PlanCacheIndexabilityState {
        MONGO_DISALLOW_COPYING(PlanCacheIndexabilityState);
    public:
        PlanCacheIndexabilityState() = default;

        /**
         * Gets the set of discriminators associated with 'path'.  Returns an empty set if no
         * discriminators are registered for 'path'.
         *
         * The object returned by reference is valid until the next call to updateDiscriminators()
         * or until destruction of 'this', whichever is first.
         */
        const IndexabilityDiscriminators& getDiscriminators(StringData path) const;

        /**
         * Clears discriminators for all paths, and regenerate them from 'indexEntries'.
         */
        void updateDiscriminators(const std::vector<IndexEntry>& indexEntries);

    private:
        /**
         * Adds sparse index discriminators for the sparse index with the given key pattern to
         * '_pathDiscriminatorsMap'.
         *
         * A sparse index discriminator distinguishes equality matches to null from other expression
         * types.  For example, this allows the predicate {a: 1} to be considered of a different
         * shape from the predicate {a: null}, if there is a sparse index defined with "a" as an
         * element of the key pattern.  The former predicate is compatibile with this index, but the
         * latter is not compatible.
         */
        void processSparseIndex(const BSONObj& keyPattern);

        /**
         * Adds partial index discriminators for the partial index with the given filter expression
         * to '_pathDiscriminatorsMap'.
         *
         * A partial index discriminator distinguishes expressions that match a given partial index
         * predicate from expressions that don't match the partial index predicate.  For example,
         * this allows the predicate {a: {$gt: 5}} to be considered a different shape than the
         * predicate {a: {$gt: -5}}, if there is a partial index defined with document filter {a:
         * {$gt: 0}}.  The former is compatible with this index, but the latter is not compatible.
         */
        void processPartialIndex(const MatchExpression* filterExpr);

        using PathDiscriminatorsMap = StringMap<IndexabilityDiscriminators>;
        PathDiscriminatorsMap _pathDiscriminatorsMap;
    };

}  // namespace mongo

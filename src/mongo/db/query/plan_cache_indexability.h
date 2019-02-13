/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/exec/projection_exec_agg.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObj;
class CollatorInterface;
class CompositeIndexabilityDiscriminator;
class MatchExpression;
struct CoreIndexInfo;

using IndexabilityDiscriminator = stdx::function<bool(const MatchExpression* me)>;
using IndexabilityDiscriminators = std::vector<IndexabilityDiscriminator>;
using IndexToDiscriminatorMap = StringMap<CompositeIndexabilityDiscriminator>;

/**
 * CompositeIndexabilityDiscriminator holds all indexability discriminators for a particular path,
 * for a particular index. For example, a path may have both a collation discriminator and a sparse
 * index discriminator for a particular index.
 */
class CompositeIndexabilityDiscriminator {
public:
    /**
     * Considers all discriminators for the path-index pair, and returns a single bit indicating
     * whether the index can be used for that path.
     */
    bool isMatchCompatibleWithIndex(const MatchExpression* me) const {
        for (auto&& discriminator : _discriminators) {
            if (!discriminator(me)) {
                return false;
            }
        }
        return true;
    }

    void addDiscriminator(IndexabilityDiscriminator discriminator) {
        _discriminators.push_back(std::move(discriminator));
    }

private:
    IndexabilityDiscriminators _discriminators;
};

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
     * Returns a map from index name to discriminator for each index associated with 'path'.
     * Returns an empty set if no discriminators are registered for 'path'.
     *
     * The object returned by reference is valid until the next call to updateDiscriminators() or
     * until destruction of 'this', whichever is first.
     */
    const IndexToDiscriminatorMap& getDiscriminators(StringData path) const;

    /**
     * Construct an IndexToDiscriminator map for the given path, only for the wildcard indexes
     * which have been included in the indexability state.
     */
    IndexToDiscriminatorMap buildWildcardDiscriminators(StringData path) const;

    /**
     * Clears discriminators for all paths, and regenerates them from 'indexCores'.
     */
    void updateDiscriminators(const std::vector<CoreIndexInfo>& indexCores);

private:
    using PathDiscriminatorsMap = StringMap<IndexToDiscriminatorMap>;

    /**
     * A $** index may index an infinite number of fields. We cannot just store a discriminator for
     * every possible field that it indexes, so we have to maintain some special context about the
     * index.
     */
    struct WildcardIndexDiscriminatorContext {
        WildcardIndexDiscriminatorContext(const ProjectionExecAgg* proj,
                                          std::string name,
                                          const MatchExpression* filter,
                                          const CollatorInterface* coll)
            : projectionExec(proj),
              filterExpr(filter),
              collator(coll),
              catalogName(std::move(name)) {}

        // These are owned by the catalog.
        const ProjectionExecAgg* projectionExec;
        const MatchExpression* filterExpr;  // For partial indexes.
        const CollatorInterface* collator;

        std::string catalogName;
    };

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
    void processSparseIndex(const std::string& indexName, const BSONObj& keyPattern);

    /**
     * Adds partial index discriminators for the partial index with the given filter expression
     * to the discriminators for that index in '_pathDiscriminatorsMap'.
     *
     * A partial index discriminator distinguishes expressions that match a given partial index
     * predicate from expressions that don't match the partial index predicate.  For example,
     * this allows the predicate {a: {$gt: 5}} to be considered a different shape than the
     * predicate {a: {$gt: -5}}, if there is a partial index defined with document filter {a:
     * {$gt: 0}}.  The former is compatible with this index, but the latter is not compatible.
     */
    void processPartialIndex(const std::string& indexName, const MatchExpression* filterExpr);

    /**
     * Adds collation discriminators for the index with the given key pattern and collator to
     * '_pathDiscriminatorsMap'.
     *
     * The discriminator for a given path returns true if the index collator matches the query
     * collator, or if the query does not contain string comparison at that path.
     */
    void processIndexCollation(const std::string& indexName,
                               const BSONObj& keyPattern,
                               const CollatorInterface* collator);

    /**
     * Adds special state for a $** index. When the discriminators are retrieved for a certain
     * path, appropriate discriminators for the wildcard index will be included if it includes the
     * given path.
     */
    void processWildcardIndex(const CoreIndexInfo& cii);

    // PathDiscriminatorsMap is a map from field path to index name to IndexabilityDiscriminator.
    PathDiscriminatorsMap _pathDiscriminatorsMap;

    std::vector<WildcardIndexDiscriminatorContext> _wildcardIndexDiscriminators;
};

}  // namespace mongo

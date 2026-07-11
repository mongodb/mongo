// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
class BSONObj;
class CollatorInterface;

class CompositeIndexabilityDiscriminator;
class MatchExpression;
struct CoreIndexInfo;

namespace projection_executor {
class ProjectionExecutor;
}

using IndexabilityDiscriminator = std::function<bool(const MatchExpression* me)>;
using IndexabilityDiscriminators = std::vector<IndexabilityDiscriminator>;
using IndexToDiscriminatorMap = StringMap<CompositeIndexabilityDiscriminator>;
using PathDiscriminatorsMap = StringMap<IndexToDiscriminatorMap>;

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
 * PlanCacheIndexabilityState holds a set of "indexability discriminators. An indexability
 * discriminator is a binary predicate function, used to classify match expressions based on the
 * data values in the expression.
 *
 * These discriminators are used to distinguish between queries of a similar shape but not the same
 * candidate indexes. So each discriminator typically represents a decision like "is this index
 * valid?" or "does this piece of the query disqualify it from using this index?". The output of
 * these decisions is included in the plan cache key.
 */
class PlanCacheIndexabilityState {
    PlanCacheIndexabilityState(const PlanCacheIndexabilityState&) = delete;
    PlanCacheIndexabilityState& operator=(const PlanCacheIndexabilityState&) = delete;

public:
    PlanCacheIndexabilityState() = default;

    /**
     * Returns a map from index name to discriminator for each index associated with 'path'.
     * Returns an empty set if no discriminators are registered for 'path'.
     *
     * The object returned by reference is valid until the next call to updateDiscriminators() or
     * until destruction of 'this', whichever is first.
     */
    const IndexToDiscriminatorMap& getPathDiscriminators(std::string_view path) const;

    /**
     * Returns a map of index name to discriminator set. These discriminators are not
     * associated with a particular path of a query and apply to the entire MatchExpression.
     */
    const IndexToDiscriminatorMap& getGlobalDiscriminators() const {
        return _globalDiscriminatorMap;
    }

    /**
     * Construct an IndexToDiscriminator map for the given path, only for the wildcard indexes
     * which have been included in the indexability state.
     */
    IndexToDiscriminatorMap buildWildcardDiscriminators(std::string_view path) const;

    /**
     * Clears discriminators for all paths, and regenerates them from 'indexCores'.
     */
    void updateDiscriminators(const std::vector<CoreIndexInfo>& indexCores);

private:
    /**
     * A $** index may index an infinite number of fields. We cannot just store a discriminator for
     * every possible field that it indexes, so we have to maintain some special context about the
     * index.
     */
    struct WildcardIndexDiscriminatorContext {
        WildcardIndexDiscriminatorContext(projection_executor::ProjectionExecutor* proj,
                                          std::string name,
                                          const CollatorInterface* coll)
            : projectionExec(proj), collator(coll), catalogName(std::move(name)) {}

        // These are owned by the catalog.
        projection_executor::ProjectionExecutor* projectionExec;
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
     * Adds a global discriminator for the partial index with the given filter expression
     * to the discriminators for that index in '_globalDiscriminatorMap'.
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

    // Map from index name to global discriminators. These are discriminators which do not apply to
    // a single path but the entire MatchExpression.
    IndexToDiscriminatorMap _globalDiscriminatorMap;

    std::vector<WildcardIndexDiscriminatorContext> _wildcardIndexDiscriminators;
};

}  // namespace mongo

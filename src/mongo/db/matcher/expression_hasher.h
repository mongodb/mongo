// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Describes whether we include parameter IDs or values from the query in the hash.
 */
enum HashValuesOrParams {
    // Hash parameter IDs where parameters are present.
    kHashParamIds = 1 << 1,
    // Hash values from the query.
    kHashValues = 1 << 2,
    // Hash index tags.
    kHashIndexTags = 1 << 3,
};

struct MatchExpression::HashParam {
    const HashValuesOrParams hashValuesOrParams;
    // Required if 'kHashIndexTags' is set. Index tags refer to indexes by their positions in this
    // list. However we don't want index tags' hashes to change based on this property. For
    // instance, we don't prune indexes when planning from cache, which could cause the relevant
    // indexes' positions to be different. Hence we hash the index identifiers instead.
    const std::vector<IndexEntry>* const indexes = nullptr;
    // 'maxNumberOfInElementsToHash' is the maximum number of equalities or regexes to hash to avoid
    // performance issues related to hashing of large '$in's.
    const size_t maxNumberOfInElementsToHash = 20;
};

/**
 * MatchExpression's hash function designed to be consistent with `MatchExpression::equivalent()`.
 * The function does not support $jsonSchema and will tassert() if provided an input that contains
 * any $jsonSchema-related nodes.
 */
size_t calculateHash(const MatchExpression& expr, const MatchExpression::HashParam& param);

/**
 * MatchExpression's hash functor implementation compatible with unordered containers. Designed to
 * be consistent with 'MatchExpression::equivalent()'. The functor does not support $jsonSchema and
 * will tassert() if provided an input that contains any $jsonSchema-related nodes.
 */
struct MatchExpressionHasher {
    explicit MatchExpressionHasher(MatchExpression::HashParam params =
                                       MatchExpression::HashParam{HashValuesOrParams::kHashValues})
        : _params(std::move(params)) {}

    size_t operator()(const MatchExpression* expr) const {
        return calculateHash(*expr, _params);
    }

private:
    const MatchExpression::HashParam _params;
};

/**
 * MatchExpression's equality functor implementation compatible with unordered containers. It uses
 * 'MatchExpression::equivalent()' under the hood and compatible with 'MatchExpressionHasher'
 * defined above.
 */
struct MatchExpressionEq {
    bool operator()(const MatchExpression* lhs, const MatchExpression* rhs) const {
        return lhs->equivalent(rhs);
    }
};

}  // namespace mongo

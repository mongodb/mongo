/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"

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

/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/canonical_query_encoder.h"

namespace mongo {
/**
 * Represents the "key" used in the PlanCache mapping from query shape -> query plan.
 */
class PlanCacheKey {
public:
    PlanCacheKey(CanonicalQuery::QueryShapeString shapeString, std::string indexabilityString) {
        _lengthOfQueryShape = shapeString.size();
        _key = std::move(shapeString);
        _key += indexabilityString;
    }

    CanonicalQuery::QueryShapeString getQueryShape() const {
        return std::string(_key, 0, _lengthOfQueryShape);
    }

    StringData getQueryShapeStringData() const {
        return StringData(_key.c_str(), _lengthOfQueryShape);
    }

    /**
     * Return the 'indexability discriminators', that is, the plan cache key component after the
     * stable key, but before the boolean indicating whether we are using the classic engine.
     */
    StringData getIndexabilityDiscriminators() const {
        return StringData(_key.c_str() + _lengthOfQueryShape, _key.size() - _lengthOfQueryShape);
    }

    StringData stringData() const {
        return _key;
    }

    const std::string& toString() const {
        return _key;
    }

    bool operator==(const PlanCacheKey& other) const {
        return other._key == _key && other._lengthOfQueryShape == _lengthOfQueryShape;
    }

    bool operator!=(const PlanCacheKey& other) const {
        return !(*this == other);
    }

    uint32_t queryHash() const {
        return canonical_query_encoder::computeHash(getQueryShapeStringData());
    }

    uint32_t planCacheKeyHash() const {
        return canonical_query_encoder::computeHash(stringData());
    }

private:
    // Key is broken into two parts:
    // <query shape key> | <indexability discriminators>
    std::string _key;

    // How long the "query shape" is.
    size_t _lengthOfQueryShape;
};

std::ostream& operator<<(std::ostream& stream, const PlanCacheKey& key);
StringBuilder& operator<<(StringBuilder& builder, const PlanCacheKey& key);

class PlanCacheKeyHasher {
public:
    std::size_t operator()(const PlanCacheKey& k) const {
        return std::hash<std::string>{}(k.toString());
    }
};
}  // namespace mongo

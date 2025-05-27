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

#include "mongo/base/string_data.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_hash.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <boost/container_hash/hash.hpp>

namespace mongo {
/**
 * Encapsulates Plan Cache key - related information used to lookup entries in the PlanCache.
 */
class PlanCacheKeyInfo {
public:
    PlanCacheKeyInfo(CanonicalQuery::QueryShapeString shapeString,
                     const std::string& indexabilityString,
                     query_settings::QuerySettings querySettings)
        : _lengthOfQueryShape{shapeString.size()}, _querySettings{std::move(querySettings)} {
        _key = std::move(shapeString);
        _key += indexabilityString;
    }

    CanonicalQuery::QueryShapeString getQueryShape() const {
        return std::string(_key, 0, _lengthOfQueryShape);
    }

    bool operator==(const PlanCacheKeyInfo& other) const = default;
    bool operator!=(const PlanCacheKeyInfo& other) const = default;

    uint32_t planCacheShapeHash() const {
        return canonical_query_encoder::computeHash(getQueryShapeStringData());
    }

    uint32_t planCacheKeyHash() const {
        size_t hash = canonical_query_encoder::computeHash(stringData());
        boost::hash_combine(hash, query_settings::hash(_querySettings));
        return hash;
    }

    const std::string& toString() const {
        return _key;
    }

    size_t keySizeInBytes() const {
        return _key.size();
    }

    /**
     * Return the 'indexability discriminators', that is, the plan cache key component after the
     * stable key, but before the boolean indicating whether we are using the classic engine.
     */
    StringData getIndexabilityDiscriminators() const {
        return StringData(_key.c_str() + _lengthOfQueryShape, _key.size() - _lengthOfQueryShape);
    }

    StringData getQueryShapeStringData() const {
        return StringData(_key.c_str(), _lengthOfQueryShape);
    }

    StringData stringData() const {
        return _key;
    }

    const query_settings::QuerySettings& querySettings() const {
        return _querySettings;
    }

private:
    // Key is broken into two parts:
    // <query shape key> | <indexability discriminators>
    std::string _key;

    // How long the "query shape" is.
    const size_t _lengthOfQueryShape;

    // QuerySettings are part of the PlanCacheKey for both classic and SBE plan caches and because
    // of that are stored in PlanCacheKeyInfo.
    query_settings::QuerySettings _querySettings;
};
}  // namespace mongo

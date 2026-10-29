// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_hash.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
    std::string_view getIndexabilityDiscriminators() const {
        return std::string_view(_key.c_str() + _lengthOfQueryShape,
                                _key.size() - _lengthOfQueryShape);
    }

    std::string_view getQueryShapeStringData() const {
        return std::string_view(_key.c_str(), _lengthOfQueryShape);
    }

    std::string_view stringData() const {
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

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/index_version.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/string_map.h"

namespace mongo {

using IndexCatalogTypeMap = StringMap<IndexCatalogType>;

class ShardingIndexesCatalogCache {
public:
    ShardingIndexesCatalogCache(CollectionIndexes collectionIndexes, IndexCatalogTypeMap&& indexes)
        : _collectionIndexes(std::move(collectionIndexes)), _indexes(std::move(indexes)) {}

    bool empty() const;

    CollectionIndexes getCollectionIndexes() const;

    size_t numIndexes() const;

    bool contains(StringData name) const;

    void add(const IndexCatalogType& index, const CollectionIndexes& collectionIndexes);

    void remove(StringData name, const CollectionIndexes& collectionIndexes);

    template <typename Callable>
    void forEachIndex(Callable&& handler) const {
        for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
            if (!handler(it->second))
                return;
        }
    }

    template <typename Callable>
    void forEachGlobalIndex(Callable&& handler) const {
        for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
            auto options = IndexOptionsType::parse(IDLParserContext("forEachGlobalIndexCtx"),
                                                   it->second.getOptions());
            if (options.getGlobal() && !handler(it->second)) {
                return;
            }
        }
    }

private:
    CollectionIndexes _collectionIndexes;
    IndexCatalogTypeMap _indexes;
};

/**
 * Constructed to be used exclusively by the CatalogCache as a vector clock (Time) to drive
 * IndexCache's lookups.
 *
 * This class wraps an IndexVersion timestamp with a disambiguating sequence number and a forced
 * refresh sequence number. When a collection is dropped and recreated, the index version resets to
 * boost::none. This cannot be compared with a valid timestamp, so the disambiguatingSequenceNumber
 * is used to compare those cases.
 *
 * Two ComparableIndexVersions with no indexes (boost::none timestamp) will always be the same
 * regardless of the collection they refer to.
 */
class ComparableIndexVersion {
public:
    /**
     * Creates a ComparableIndexVersion that wraps the CollectionIndexes' timestamp.
     */
    static ComparableIndexVersion makeComparableIndexVersion(
        const boost::optional<Timestamp>& version);

    /**
     * Creates a new instance which will artificially be greater than any previously created
     * ComparableIndexVersion and smaller than any instance created afterwards. Used as means to
     * cause the collections cache to attempt a refresh in situations where causal consistency
     * cannot be inferred.
     */
    static ComparableIndexVersion makeComparableIndexVersionForForcedRefresh();

    /**
     * Empty constructor needed by the ReadThroughCache.
     */
    ComparableIndexVersion() = default;

    std::string toString() const;

    bool operator==(const ComparableIndexVersion& other) const;

    bool operator!=(const ComparableIndexVersion& other) const {
        return !(*this == other);
    }

    /**
     * Two boost::none timestamps will always evaluate as equal. If one version is boost::none and
     * the other has a timestamp, the comparison will depend on the disambiguating sequence number.
     */
    bool operator<(const ComparableIndexVersion& other) const;

    bool operator>(const ComparableIndexVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableIndexVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableIndexVersion& other) const {
        return !(*this < other);
    }

private:
    friend class CatalogCache;

    static AtomicWord<uint64_t> _disambiguatingSequenceNumSource;
    static AtomicWord<uint64_t> _forcedRefreshSequenceNumSource;

    ComparableIndexVersion(uint64_t forcedRefreshSequenceNum,
                           boost::optional<Timestamp> version,
                           uint64_t disambiguatingSequenceNum)
        : _forcedRefreshSequenceNum(forcedRefreshSequenceNum),
          _indexVersion(std::move(version)),
          _disambiguatingSequenceNum(disambiguatingSequenceNum) {}

    void setCollectionIndexes(const boost::optional<Timestamp>& version);

    uint64_t _forcedRefreshSequenceNum{0};

    boost::optional<Timestamp> _indexVersion;

    // Locally incremented sequence number that allows to compare two versions where one is
    // boost::none and the other has a timestamp. Two boost::none versions with different
    // disambiguating sequence numbers are still considered equal.
    uint64_t _disambiguatingSequenceNum{0};
};

/**
 * This intermediate structure is necessary to be able to store collections without any global
 * indexes in the cache. The cache does not allow for an empty value, so this intermediate structure
 * is needed.
 */
struct OptionalShardingIndexCatalogInfo {
    // No indexes constructor.
    OptionalShardingIndexCatalogInfo() = default;

    // Constructor with global indexes
    OptionalShardingIndexCatalogInfo(ShardingIndexesCatalogCache sii) : optSii(std::move(sii)) {}

    // If nullptr, the collection has an index version of boost::none and no global indexes.
    // Otherwise, the index version is some valid timestamp (there still may be no global indexes).
    boost::optional<ShardingIndexesCatalogCache> optSii;
};

using ShardingIndexesCatalogRTCBase =
    ReadThroughCache<NamespaceString, OptionalShardingIndexCatalogInfo, ComparableIndexVersion>;

}  // namespace mongo

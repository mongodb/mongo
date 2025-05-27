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

#include "mongo/db/index_names.h"
#include "mongo/platform/atomic_word.h"

#include <functional>
#include <map>
#include <string>

namespace mongo {
class IndexDescriptor;
class ServiceContext;

/**
 * IndexFeatures describes an anonymized set of features about a single index. For example, an index
 * can be both compound and unique, and this set of flags would be used to track that information so
 * that we can provide aggregated details in the AggregatedIndexUsageTracker.
 */
struct IndexFeatures {
    /**
     * Create an IndexFeatures structure. If 'internal' is true, the statistics for this index and
     * its features should not be tracked and aggregated by the AggregatedIndexUsageTracker.
     */
    static IndexFeatures make(const IndexDescriptor* desc, bool internal);

    IndexType type;
    bool collation = false;
    bool compound = false;
    bool id = false;
    bool internal = false;
    bool partial = false;
    bool prepareUnique = false;
    bool sparse = false;
    bool ttl = false;
    bool unique = false;
};

/**
 * IndexFeatureStats holds statistics about a specific index feature. Its data members are mutable
 * atomics to allow itself to be used in a const map safely.
 */
struct IndexFeatureStats {
    // Number of indexes that have this feature.
    mutable AtomicWord<long long> count{0};
    // Number of operations that have used indexes with this feature.
    mutable AtomicWord<long long> accesses{0};
};

enum class FeatureStatType {
    kCollation,
    kCompound,
    kId,
    kInternal,
    kPartial,
    kPrepareUnique,
    kSingle,
    kSparse,
    kTTL,
    kUnique,
    kCount,
};

/**
 * AggregatedIndexUsageTracker aggregates usage metrics about features used by indexes. Ignores
 * indexes on internal databases.
 */
class AggregatedIndexUsageTracker {
public:
    using IndexStatsType = std::array<IndexFeatureStats, INDEX_TYPE_COUNT>;
    using FeatureStatsType =
        std::array<IndexFeatureStats, static_cast<size_t>(FeatureStatType::kCount)>;

    static AggregatedIndexUsageTracker* get(ServiceContext* svcCtx);

    AggregatedIndexUsageTracker();

    /**
     * Updates counters for features used by an index when the index has been accessed.
     */
    void onAccess(const IndexFeatures& features) const;

    /**
     * Updates counters for indexes using certain features when the index has been created.
     */
    void onRegister(const IndexFeatures& features) const;

    /**
     * Updates counters for indexes using certain features when the index has been removed.
     */
    void onUnregister(const IndexFeatures& features) const;

    /**
     * Iterates through each feature being tracked with a call back to OnFeatureFn, which provides
     * the string descriptor of the feature and its stats.
     */
    using OnFeatureFn =
        std::function<void(const std::string& feature, const IndexFeatureStats& stats)>;
    void forEachFeature(OnFeatureFn&& onFeature) const;

    /**
     * Returns the total number of indexes being tracked.
     */
    long long getCount() const;

    /**
     * Reset all stats to zero.
     */
    void resetToZero();

private:
    mutable IndexStatsType _indexTypeStats;
    mutable FeatureStatsType _featureStats;

    // Total number of indexes being tracked.
    mutable AtomicWord<long long> _count;
};
}  // namespace mongo

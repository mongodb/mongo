/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <cstddef>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

class BucketCatalogServerStatus : public ServerStatusSection {
    struct BucketCounts {
        BucketCounts& operator+=(const BucketCounts& other) {
            if (&other != this) {
                all += other.all;
                open += other.open;
                idle += other.idle;
            }
            return *this;
        }

        std::size_t all = 0;
        std::size_t open = 0;
        std::size_t idle = 0;
    };

    BucketCounts _getBucketCounts(const BucketCatalog& catalog) const {
        BucketCounts sum;
        for (auto const& stripe : catalog.stripes) {
            stdx::lock_guard stripeLock{stripe->mutex};
            sum += {stripe->openBucketsById.size(),
                    stripe->openBucketsByKey.size(),
                    stripe->idleBuckets.size()};
        }
        return sum;
    }

public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            stdx::lock_guard catalogLock{bucketCatalog.mutex};
            if (bucketCatalog.executionStats.empty()) {
                return {};
            }
        }

        auto counts = _getBucketCounts(bucketCatalog);
        auto numActive = bucketCatalog.numberOfActiveBuckets.load();
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(numActive));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("numArchivedBuckets", static_cast<long long>(numActive - counts.open));
        builder.appendNumber("memoryUsage", static_cast<long long>(getMemoryUsage(bucketCatalog)));

        // Append the global execution stats for all namespaces.
        appendExecutionStatsToBuilder(bucketCatalog.globalExecutionStats, builder);

        // Append the global state management stats for all namespaces.
        appendStats(bucketCatalog.bucketStateRegistry, builder);

        return builder.obj();
    }
};
auto& bucketCatalogServerStatus =
    *ServerStatusSectionBuilder<BucketCatalogServerStatus>("bucketCatalog").forShard();

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog

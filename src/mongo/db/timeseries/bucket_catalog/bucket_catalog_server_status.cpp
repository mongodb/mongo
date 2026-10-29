// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"

#include <algorithm>
#include <cstddef>

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
            sum += {static_cast<std::size_t>(stripe->numOpenBucketsByIdCount.loadRelaxed()),
                    static_cast<std::size_t>(stripe->numOpenBucketsByKeyCount.loadRelaxed()),
                    static_cast<std::size_t>(stripe->numIdleBucketsCount.loadRelaxed())};
        }
        return sum;
    }

public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = GlobalBucketCatalog::get(opCtx->getServiceContext());
        if (bucketCatalog.numExecutionStatsEntries.loadRelaxed() == 0) {
            return {};
        }

        auto counts = _getBucketCounts(bucketCatalog);
        auto numActive = bucketCatalog.globalExecutionStats.numActiveBuckets.loadRelaxed();
        // Because counts.open and numActive are loaded without locks, counts.open can transiently
        // exceed numActive. Use signed arithmetic and clamp at 0 to avoid unsigned wraparound.
        const long long numArchived =
            std::max<long long>(0, numActive - static_cast<long long>(counts.open));
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(numActive));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("numArchivedBuckets", numArchived);
        builder.appendNumber("memoryUsage", static_cast<long long>(getMemoryUsage(bucketCatalog)));
        getDetailedMemoryUsage(bucketCatalog, builder);

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

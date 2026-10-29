// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/shard_catalog/catalog_stats.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/views_for_database.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo::catalog_stats {

// Number of time-series collections requiring extended range support
Atomic<int> requiresTimeseriesExtendedRangeSupport;

namespace {
class CatalogStatsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~CatalogStatsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    struct Stats {
        int collections = 0;
        int capped = 0;
        int clustered = 0;
        int views = 0;
        int timeseries = 0;
        int internalCollections = 0;
        int internalViews = 0;
        int timeseriesExtendedRange = 0;
        int csfle = 0;
        int queryableEncryption = 0;
        int systemProfile = 0;

        void toBson(BSONObjBuilder* builder) const {
            builder->append("collections", collections);
            builder->append("capped", capped);
            builder->append("clustered", clustered);
            builder->append("timeseries", timeseries);
            builder->append("views", views);
            builder->append("internalCollections", internalCollections);
            builder->append("internalViews", internalViews);
            if (timeseriesExtendedRange > 0) {
                builder->append("timeseriesExtendedRange", timeseriesExtendedRange);
            }
            builder->append("csfle", csfle);
            builder->append("queryableEncryption", queryableEncryption);
            builder->append("systemProfile", systemProfile);
        }
    };

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        Stats stats;

        const auto catalog = CollectionCatalog::get(opCtx);
        const auto catalogStats = catalog->getStats();
        stats.collections = catalogStats.userCollections;
        stats.capped = catalogStats.userCapped;
        stats.clustered = catalogStats.userClustered;
        stats.timeseries = catalogStats.userTimeseries;
        stats.internalCollections = catalogStats.internal;
        stats.timeseriesExtendedRange = requiresTimeseriesExtendedRangeSupport.loadRelaxed();
        stats.csfle = catalogStats.csfle;
        stats.queryableEncryption = catalogStats.queryableEncryption;
        stats.systemProfile = catalogStats.systemProfile;

        const auto viewCatalogDbNames = catalog->getViewCatalogDbNames(opCtx);
        for (const auto& dbName : viewCatalogDbNames) {
            const auto viewStats = catalog->getViewStatsForDatabase(opCtx, dbName);
            invariant(viewStats);

            stats.timeseries += viewStats->userTimeseries;
            stats.views += viewStats->userViews;
            stats.internalViews += viewStats->internal;
        }

        BSONObjBuilder builder;
        stats.toBson(&builder);
        return builder.obj();
    }
};
auto catalogStatsSSS = *ServerStatusSectionBuilder<CatalogStatsSSS>("catalogStats").forShard();
}  // namespace
}  // namespace mongo::catalog_stats

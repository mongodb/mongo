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


#include "mongo/db/local_catalog/catalog_stats.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/views_for_database.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo::catalog_stats {

// Number of time-series collections requiring extended range support
AtomicWord<int> requiresTimeseriesExtendedRangeSupport;

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

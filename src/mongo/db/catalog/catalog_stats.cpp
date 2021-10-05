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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {
class CatalogStatsSSS : public ServerStatusSection {
public:
    CatalogStatsSSS() : ServerStatusSection("catalogStats") {}

    ~CatalogStatsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    struct Stats {
        int collections = 0;
        int capped = 0;
        int views = 0;
        int timeseries = 0;
        int internalCollections = 0;
        int internalViews = 0;

        void toBson(BSONObjBuilder* builder) const {
            builder->append("collections", collections);
            builder->append("capped", capped);
            builder->append("timeseries", timeseries);
            builder->append("views", views);
            builder->append("internalCollections", internalCollections);
            builder->append("internalViews", internalViews);
        }
    };

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        Stats stats;

        const auto catalog = CollectionCatalog::get(opCtx);
        const auto catalogStats = catalog->getStats();
        stats.collections = catalogStats.userCollections;
        stats.capped = catalogStats.userCapped;
        stats.internalCollections = catalogStats.internal;

        const auto viewCatalogDbNames = catalog->getViewCatalogDbNames();
        for (const auto& dbName : viewCatalogDbNames) {
            try {
                const auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, dbName);
                if (!viewCatalog) {
                    // The database may have been dropped between listing the database names and
                    // looking up the view catalog.
                    continue;
                }
                const auto viewStats = viewCatalog->getStats();
                stats.timeseries += viewStats.userTimeseries;
                stats.views += viewStats.userViews;
                stats.internalViews += viewStats.internal;
            } catch (ExceptionForCat<ErrorCategory::Interruption>&) {
                LOGV2_DEBUG(5578400,
                            2,
                            "Failed to collect view catalog statistics",
                            "dbName"_attr = dbName);
            }
        }

        BSONObjBuilder builder;
        stats.toBson(&builder);
        return builder.obj();
    }

} catalogStatsSSS;
}  // namespace
}  // namespace mongo

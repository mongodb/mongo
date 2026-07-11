// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/stats_cache_loader_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/stats_for_histograms_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stats {
SemiFuture<StatsCacheVal> StatsCacheLoaderImpl::getStats(OperationContext* opCtx,
                                                         const StatsPathString& statsPath) {

    std::string statsColl(std::string{kStatsPrefix} + "." + std::string{statsPath.first.coll()});

    const auto statsNss = NamespaceStringUtil::deserialize(statsPath.first.dbName(), statsColl);
    DBDirectClient client(opCtx);


    FindCommandRequest findRequest{statsNss};
    BSONObj filter = BSON("_id" << statsPath.second);
    LOGV2_DEBUG(7085600, 1, "findRequest filter", "filter"_attr = redact(filter.toString()));
    findRequest.setFilter(filter.getOwned());

    try {
        auto cursor = client.find(std::move(findRequest));

        if (!cursor) {
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to establish a cursor for reading "
                                    << statsPath.first.toStringForErrorMsg() << ",  path "
                                    << statsPath.second << " from local storage");
        }

        if (cursor->more()) {
            IDLParserContext ctx("StatsPath");
            BSONObj document = cursor->nextSafe().getOwned();
            auto parsedStats = StatsPath::parse(document, ctx);
            StatsCacheVal statsPtr(CEHistogram::make(parsedStats.getStatistics()));
            return makeReadyFutureWith([this, statsPtr] { return statsPtr; }).semi();
        }

        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "Stats does not exists for " << statsNss.toStringForErrorMsg()
                                << ",  path " << statsPath.second);
    } catch (const DBException& ex) {
        uassertStatusOK(ex.toStatus());
    }
    MONGO_UNREACHABLE
}

}  // namespace mongo::stats

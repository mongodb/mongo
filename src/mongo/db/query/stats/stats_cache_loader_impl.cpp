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

#include "mongo/db/query/stats/stats_cache_loader_impl.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/stats/stats_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stats {
SemiFuture<StatsCacheVal> StatsCacheLoaderImpl::getStats(OperationContext* opCtx,
                                                         const StatsPathString& statsPath) {

    std::string statsColl(kStatsPrefix + "." + statsPath.first.coll());

    NamespaceString statsNss(statsPath.first.db(), statsColl);
    DBDirectClient client(opCtx);


    FindCommandRequest findRequest{statsNss};
    BSONObj filter = BSON("_id" << statsPath.second);
    LOGV2_DEBUG(7085600, 1, "findRequest filter", "filter"_attr = filter.toString());
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
            auto parsedStats = StatsPath::parse(ctx, document);
            StatsCacheVal statsPtr(ArrayHistogram::make(parsedStats.getStatistics()));
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

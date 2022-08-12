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


#include "mongo/platform/basic.h"

#include "mongo/db/query/ce/stats_cache_loader_impl.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/collection_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"

namespace mongo {


SemiFuture<CollectionStatistics> StatsCacheLoaderImpl::getStats(OperationContext* opCtx,
                                                                const NamespaceString& nss) {

    std::string statsColl(kStatsPrefix + "." + nss.ns());

    NamespaceString statsNss(kStatsDb, statsColl);
    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{statsNss};
    BSONObj result;

    try {
        auto cursor = client.find(findRequest);

        if (!cursor) {
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to establish a cursor for reading " << nss.ns()
                                    << " from local storage");
        }

        std::vector<BSONObj> histograms;
        while (cursor->more()) {
            BSONObj document = cursor->nextSafe().getOwned();
            histograms.push_back(std::move(document));
        }

        // TODO: SERVER-68745, parse histograms BSONs.
        CollectionStatistics stats{0};
        return makeReadyFutureWith([this, stats] { return stats; }).semi();
    } catch (const DBException& ex) {
        uassertStatusOK(ex.toStatus());
    }
    MONGO_UNREACHABLE
}

}  // namespace mongo

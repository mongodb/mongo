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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>


namespace mongo::stats {
using StatsPathString = std::pair<NamespaceString, std::string>;
using StatsCacheVal = std::shared_ptr<const CEHistogram>;

class StatsCacheLoader {
public:
    /**
     * Non-blocking call, which returns CollectionStatistics from the the persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<StatsCacheVal> getStats(OperationContext* opCtx,
                                               const StatsPathString& statsPath) = 0;

    virtual void setStatsReturnValueForTest(StatusWith<StatsCacheVal> swStats) {};

    virtual ~StatsCacheLoader() {}

    static constexpr StringData kStatsPrefix = "system.statistics"_sd;
};

}  // namespace mongo::stats

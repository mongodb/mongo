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

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/ce/collection_statistics.h"
#include "mongo/db/query/ce/stats_cache.h"
#include "mongo/db/query/ce/stats_cache_loader.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

using namespace mongo::ce;

/**
 * This class owns statsCache and manages executor lifetime.
 */
class StatsCatalog {
public:
    /**
     * Stores the catalog on the specified service context. May only be called once for the lifetime
     * of the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<StatsCatalog> catalog);

    static StatsCatalog& get(ServiceContext* serviceContext);
    static StatsCatalog& get(OperationContext* opCtx);

    /**
     * The constructor provides the Service context under which the cache needs to be instantiated,
     * and a Thread pool to be used for invoking the blocking 'lookup' calls. The size is the number
     * of entries the underlying LRU cache will hold.
     */
    StatsCatalog(ServiceContext* service, std::unique_ptr<StatsCacheLoader> cacheLoader);

    ~StatsCatalog();

    StatusWith<std::shared_ptr<ArrayHistogram>> getHistogram(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const std::string& path);

private:
    /**
     * The executor is used by the cache.
     */
    std::shared_ptr<ThreadPool> _executor;
    StatsCache _statsCache;
};

}  // namespace mongo

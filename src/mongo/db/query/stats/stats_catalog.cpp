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

#include "mongo/db/query/stats/stats_catalog.h"

#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/collection_statistics.h"
#include "mongo/db/query/stats/stats_cache.h"
#include "mongo/util/read_through_cache.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stats {
namespace {
const auto statsCatalogDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<StatsCatalog>>();
}  // namespace

StatsCatalog::StatsCatalog(ServiceContext* service,
                           std::unique_ptr<StatsCacheLoader> statsCacheLoader)
    : _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "StatsCache";
          options.minThreads = 0;
          options.maxThreads = 2;
          return options;
      }())),
      _statsCache(service, std::move(statsCacheLoader), *_executor, 1000) {
    _executor->startup();
}

StatsCatalog::~StatsCatalog() {
    // The executor is used by the StatsCatalog, so it must be joined, before this cache is
    // destroyed, per the contract of ReadThroughCache.
    _executor->shutdown();
    _executor->join();
}

void StatsCatalog::set(ServiceContext* serviceContext, std::unique_ptr<StatsCatalog> cache) {
    auto& statsCatalog = statsCatalogDecoration(serviceContext);
    invariant(!statsCatalog);

    statsCatalog = std::move(cache);
}

StatsCatalog& StatsCatalog::get(ServiceContext* serviceContext) {
    auto& statsCatalog = statsCatalogDecoration(serviceContext);
    invariant(statsCatalog);

    return *statsCatalog;
}

StatsCatalog& StatsCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

StatusWith<std::shared_ptr<const ArrayHistogram>> StatsCatalog::getHistogram(
    OperationContext* opCtx, const NamespaceString& nss, const std::string& path) {
    try {
        auto handle = _statsCache.acquire(opCtx, std::make_pair(nss, path));
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "path " << nss.toStringForErrorMsg() << " : " << path
                              << " not found",
                handle);

        return *(handle.get());
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status StatsCatalog::invalidatePath(const NamespaceString& nss, const std::string& path) {
    try {
        _statsCache.invalidateKey(std::make_pair(nss, path));
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}
}  // namespace mongo::stats

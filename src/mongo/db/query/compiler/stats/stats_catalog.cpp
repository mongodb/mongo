// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/stats_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/stats_cache.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stats {
namespace {
const auto statsCatalogDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<StatsCatalog>>();
}  // namespace

StatsCatalog::StatsCatalog(Service* service, std::unique_ptr<StatsCacheLoader> statsCacheLoader)
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

StatusWith<std::shared_ptr<const CEHistogram>> StatsCatalog::getHistogram(
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

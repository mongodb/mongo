// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/routing_information_cache.h"

#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/util/decorable.h"

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

struct RoutingInformationCacheContainer {
    std::unique_ptr<RoutingInformationCache> routingInformationCache;
    CatalogCache* rawPtr{};
};

const auto getDecoration = ServiceContext::declareDecoration<RoutingInformationCacheContainer>();

const auto decorationActionRegisterer = ServiceContext::ConstructorActionRegisterer(
    "RoutingInformationCacheContainer",
    [](ServiceContext* serviceCtx) {
        /* Noop; construction will be executed through the RoutingInformationCache::set() method */
    },
    [](ServiceContext* serviceCtx) { getDecoration(serviceCtx).routingInformationCache.reset(); });

}  // namespace

RoutingInformationCache::RoutingInformationCache(ServiceContext* serviceCtx)
    : CatalogCache(serviceCtx,
                   std::make_shared<ConfigServerCatalogCacheLoaderImpl>(),
                   "ConfigServerRoutingInfo"sv /*kind*/) {}

void RoutingInformationCache::set(ServiceContext* serviceCtx) {
    auto& decoration = getDecoration(serviceCtx);
    invariant(!decoration.routingInformationCache);
    invariant(!decoration.rawPtr);
    decoration.routingInformationCache = std::make_unique<RoutingInformationCache>(serviceCtx);
    decoration.rawPtr = decoration.routingInformationCache.get();
}

void RoutingInformationCache::setOverride(ServiceContext* serviceCtx, CatalogCache* cacheOverride) {
    auto& decoration = getDecoration(serviceCtx);
    invariant(!decoration.routingInformationCache);
    invariant(!decoration.rawPtr);
    decoration.rawPtr = cacheOverride;
}

CatalogCache* RoutingInformationCache::get(ServiceContext* serviceCtx) {
    return getDecoration(serviceCtx).rawPtr;
}

CatalogCache* RoutingInformationCache::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

}  // namespace mongo

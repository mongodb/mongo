/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"

#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/util/decorable.h"

namespace mongo {

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
                   "ConfigServerRoutingInfo"_sd /*kind*/) {}

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

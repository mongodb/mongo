// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/router_role/routing_cache/catalog_cache_loader.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PARENT_PRIVATE]] ConfigServerCatalogCacheLoader : public CatalogCacheLoader {};

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

namespace catalog_cache_diagnostics_helpers {

void appendWhenUnknown(BSONObjBuilder* builder, bool fullMetadata);

void appendCatalogCacheInfo(OperationContext* opCtx,
                            BSONObjBuilder* builder,
                            const NamespaceString& nss,
                            bool fullMetadata);


void appendLatestCachedCollInfo(OperationContext* opCtx,
                                BSONObjBuilder* builder,
                                const NamespaceString& nss,
                                bool fullMetadata);

void appendLatestCachedDbInfo(OperationContext* opCtx,
                              BSONObjBuilder* builder,
                              const DatabaseName& dbName);

}  // namespace catalog_cache_diagnostics_helpers

}  // namespace mongo

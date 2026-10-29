// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/virtual_collection_impl.h"

#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/virtual_collection/external_record_store.h"
#include "mongo/db/shard_role/shard_catalog/collection_impl.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
VirtualCollectionImpl::VirtualCollectionImpl(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options,
                                             std::unique_ptr<ExternalRecordStore> recordStore)
    : _nss(nss),
      _options(options),
      _shared(std::make_shared<SharedState>(
          std::move(recordStore), CollectionImpl::parseCollation(opCtx, nss, options.collation))),
      _indexCatalog(std::make_unique<IndexCatalogImpl>()) {
    tassert(6968503,
            "Cannot create _id index for a virtual collection",
            options.autoIndexId == CollectionOptions::NO && options.idIndex.isEmpty());
}

std::shared_ptr<Collection> VirtualCollectionImpl::make(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const CollectionOptions& options,
                                                        const VirtualCollectionOptions& vopts) {
    return std::make_shared<VirtualCollectionImpl>(
        opCtx, nss, options, std::make_unique<ExternalRecordStore>(options.uuid, vopts));
}
}  // namespace mongo

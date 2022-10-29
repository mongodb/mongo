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

#include "mongo/db/catalog/virtual_collection_impl.h"

#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/external_record_store.h"

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
        opCtx, nss, options, std::make_unique<ExternalRecordStore>(nss.ns(), vopts));
}
}  // namespace mongo

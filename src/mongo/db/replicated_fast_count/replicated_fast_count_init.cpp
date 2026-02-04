/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

Status createFastcountCollection(OperationContext* opCtx) {
    try {

        WriteUnitOfWork wuow(opCtx);
        Status createCollectionStatus = createCollection(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(
                NamespaceString::kSystemReplicatedFastCountStore),
            CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()},
            BSONObj{});
        uassert(11757500,
                str::stream() << "Failed to create the replicated fast count collection: "
                              << NamespaceString::makeGlobalConfigCollection(
                                     NamespaceString::kSystemReplicatedFastCountStore)
                                     .toStringForErrorMsg()
                              << causedBy(createCollectionStatus.reason()) << "code"
                              << createCollectionStatus.code(),
                createCollectionStatus.isOK() ||
                    createCollectionStatus.code() == ErrorCodes::NamespaceExists);

        if (createCollectionStatus.isOK()) {
            LOGV2(11718601,
                  "Created internal fastcount collection.",
                  "ns"_attr = NamespaceString::makeGlobalConfigCollection(
                                  NamespaceString::kSystemReplicatedFastCountStore)
                                  .toStringForErrorMsg());
        } else if (createCollectionStatus.code() == ErrorCodes::NamespaceExists) {
            LOGV2(11886900,
                  "Internal fastcount collection already exists.",
                  "ns"_attr = NamespaceString::makeGlobalConfigCollection(
                                  NamespaceString::kSystemReplicatedFastCountStore)
                                  .toStringForErrorMsg());
        }

        wuow.commit();

    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo

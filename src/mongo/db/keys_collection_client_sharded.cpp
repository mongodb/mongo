// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/keys_collection_client_sharded.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_level.h"

#include <string_view>

namespace mongo {

KeysCollectionClientSharded::KeysCollectionClientSharded(ShardingCatalogClient* client)
    : _catalogClient(client) {}

StatusWith<std::vector<KeysCollectionDocument>> KeysCollectionClientSharded::getNewInternalKeys(
    OperationContext* opCtx,
    std::string_view purpose,
    const LogicalTime& newerThanThis,
    bool tryUseMajority) {
    return _catalogClient->getNewInternalKeys(
        opCtx, purpose, newerThanThis, repl::ReadConcernArgs::kMajority);
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
KeysCollectionClientSharded::getAllExternalKeys(OperationContext* opCtx, std::string_view purpose) {
    return _catalogClient->getAllExternalKeys(opCtx, purpose, repl::ReadConcernArgs::kMajority);
}

Status KeysCollectionClientSharded::insertNewKey(OperationContext* opCtx, const BSONObj& doc) {
    return _catalogClient->insertConfigDocument(opCtx,
                                                NamespaceString::kKeysCollectionNamespace,
                                                doc,
                                                defaultMajorityWriteConcernDoNotUse());
}

}  // namespace mongo

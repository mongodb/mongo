// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo {

class ShardingCatalogClient;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] KeysCollectionClientSharded : public KeysCollectionClient {
public:
    KeysCollectionClientSharded(ShardingCatalogClient*);

    /**
     * Returns internal keys for the given purpose and have an expiresAt value greater than
     * newerThanThis on the config server. Uses readConcern level majority if possible.
     */
    [[MONGO_MOD_PRIVATE]] StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        std::string_view purpose,
        const LogicalTime& newerThanThis,
        bool tryUseMajority) override;

    /**
     * Returns all external (i.e. validation-only) keys for the given purpose on the config server.
     */
    [[MONGO_MOD_PRIVATE]] StatusWith<std::vector<ExternalKeysCollectionDocument>>
    getAllExternalKeys(OperationContext* opCtx, std::string_view purpose) override;

    /**
     * Directly inserts a key document to the storage
     */
    [[MONGO_MOD_PRIVATE]] Status insertNewKey(OperationContext* opCtx, const BSONObj& doc) override;

    [[MONGO_MOD_PRIVATE]] bool mustUseLocalReads() const final {
        // Reads are always made against the config server with majority read concern.
        return false;
    }

private:
    ShardingCatalogClient* const _catalogClient;
};

}  // namespace mongo

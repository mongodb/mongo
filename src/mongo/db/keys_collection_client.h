// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo {

class BSONObj;
class LogicalTime;
class OperationContext;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] KeysCollectionClient {
public:
    virtual ~KeysCollectionClient() = default;

    /**
     * Returns internal keys (keys for signing and validating cluster times created by nodes in the
     * clusters that this node is in) that match the given purpose and have an expiresAt value
     * greater than newerThanThis. Uses readConcern level majority if possible.
     */
    [[MONGO_MOD_PRIVATE]] virtual StatusWith<std::vector<KeysCollectionDocument>>
    getNewInternalKeys(OperationContext* opCtx,
                       std::string_view purpose,
                       const LogicalTime& newerThanThis,
                       bool tryUseMajority) = 0;

    /**
     * Returns all external keys (validation-only keys copied from other clusters) that match the
     * given purpose.
     */
    [[MONGO_MOD_PRIVATE]] virtual StatusWith<std::vector<ExternalKeysCollectionDocument>>
    getAllExternalKeys(OperationContext* opCtx, std::string_view purpose) = 0;

    /**
     * Directly inserts a key document to the storage
     */
    [[MONGO_MOD_PRIVATE]] virtual Status insertNewKey(OperationContext* opCtx,
                                                      const BSONObj& doc) = 0;

    /**
     * Returns true if the client can only read with local read concern, which means keys read by a
     * refresh may be rolled back.
     */
    [[MONGO_MOD_PRIVATE]] virtual bool mustUseLocalReads() const = 0;
};

}  // namespace mongo

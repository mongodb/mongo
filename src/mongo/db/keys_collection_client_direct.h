// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/rs_local_client.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;
class LogicalTime;
class BSONObj;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] KeysCollectionClientDirect : public KeysCollectionClient {
public:
    KeysCollectionClientDirect(bool mustUseLocalReads);

    /**
     * Returns internal keys for the given purpose and have an expiresAt value greater than
     * newerThanThis. Uses readConcern level majority if possible.
     */
    [[MONGO_MOD_PRIVATE]] StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        std::string_view purpose,
        const LogicalTime& newerThanThis,
        bool tryUseMajority) override;

    /**
     * Returns all external (i.e. validation-only) keys for the given purpose.
     */
    [[MONGO_MOD_PRIVATE]] StatusWith<std::vector<ExternalKeysCollectionDocument>>
    getAllExternalKeys(OperationContext* opCtx, std::string_view purpose) override;

    /**
     * Directly inserts a key document to the storage
     */
    [[MONGO_MOD_PRIVATE]] Status insertNewKey(OperationContext* opCtx, const BSONObj& doc) override;

    /**
     * Returns true if getNewKeys always uses readConcern level:local, so the documents returned can
     * be rolled back.
     */
    [[MONGO_MOD_PRIVATE]] bool mustUseLocalReads() const final {
        return _mustUseLocalReads;
    }

private:
    /**
     * Returns keys in the given collection for the given purpose and have an expiresAt value
     * greater than newerThanThis, using readConcern level majority if possible.
     */
    template <typename KeyDocumentType>
    StatusWith<std::vector<KeyDocumentType>> _getNewKeys(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         std::string_view purpose,
                                                         const LogicalTime& newerThanThis,
                                                         bool tryUseMajority);

    StatusWith<Shard::QueryResponse> _query(OperationContext* opCtx,
                                            const ReadPreferenceSetting& readPref,
                                            const repl::ReadConcernArgs& readConcern,
                                            const NamespaceString& nss,
                                            const BSONObj& query,
                                            const BSONObj& sort,
                                            boost::optional<long long> limit);

    Status _insert(OperationContext* opCtx,
                   const BSONObj& doc,
                   const WriteConcernOptions& writeConcern);

    RSLocalClient _rsLocalClient;
    bool _mustUseLocalReads{false};
};
}  // namespace mongo

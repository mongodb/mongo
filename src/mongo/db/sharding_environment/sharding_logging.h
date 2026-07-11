// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

/**
 * Diagnostic logging of sharding metadata events (changelog and actionlog).
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingLogging {
public:
    /**
     * Retrieves the ShardingLogging instance associated with the current service/operation context.
     */
    static ShardingLogging* get(ServiceContext* serviceContext);
    static ShardingLogging* get(OperationContext* operationContext);

    Status logAction(OperationContext* opCtx,
                     std::string_view what,
                     const NamespaceString& ns,
                     const BSONObj& detail,
                     std::shared_ptr<Shard> configShard = nullptr,
                     ShardingCatalogClient* catalogClient = nullptr);

    Status logChangeChecked(
        OperationContext* opCtx,
        std::string_view what,
        const NamespaceString& ns,
        const BSONObj& detail = BSONObj(),
        const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse(),
        std::shared_ptr<Shard> configShard = nullptr,
        ShardingCatalogClient* catalogClient = nullptr);

    void logChange(OperationContext* const opCtx,
                   const std::string_view what,
                   const NamespaceString& ns,
                   const BSONObj& detail = BSONObj(),
                   const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse(),
                   std::shared_ptr<Shard> configShard = nullptr,
                   ShardingCatalogClient* catalogClient = nullptr) {
        // It is safe to ignore the results of `logChangeChecked` in many cases, as the
        // failure to log a change is often of no consequence.
        logChangeChecked(
            opCtx, what, ns, detail, writeConcern, std::move(configShard), catalogClient)
            .ignore();
    }

private:
    /**
     * Creates the specified collection name in the config database.
     */
    Status _createCappedConfigCollection(OperationContext* opCtx,
                                         std::string_view collName,
                                         int cappedSize,
                                         const WriteConcernOptions& writeConcern,
                                         std::shared_ptr<Shard> configShard);

    /**
     * Best effort method, which logs diagnostic events on the config server. If the config server
     * write fails for any reason a warning will be written to the local service log and the method
     * will return a failed status.
     *
     * @param opCtx Operation context in which the call is running
     * @param logCollName Which config collection to write to (excluding the database name)
     * @param what E.g. "split", "migrate" (not interpreted)
     * @param operationNS To which collection the metadata change is being applied (not interpreted)
     * @param detail Additional info about the metadata change (not interpreted)
     * @param writeConcern Write concern options to use for logging
     */
    Status _log(OperationContext* opCtx,
                std::string_view logCollName,
                std::string_view what,
                const NamespaceString& operationNSS,
                const BSONObj& detail,
                const WriteConcernOptions& writeConcern,
                ShardingCatalogClient* catalogClient);

    // Member variable properties:
    // (S) Self-synchronizing; access in any way from any context.

    // Whether the logAction call should attempt to create the actionlog collection
    Atomic<int> _actionLogCollectionCreated{0};  // (S)

    // Whether the logChange call should attempt to create the changelog collection
    Atomic<int> _changeLogCollectionCreated{0};  // (S)
};

}  // namespace mongo

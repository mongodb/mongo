// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cluster_transaction_api.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_command_translations.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <string>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <fmt/format.h>

namespace mongo::txn_api::details {

BSONObj ClusterSEPTransactionClientBehaviors::maybeModifyCommand(BSONObj cmdObj) const {
    return cluster::cmd::translations::replaceCommandNameWithClusterCommandName(cmdObj);
}

Future<DbResponse> ClusterSEPTransactionClientBehaviors::handleRequest(OperationContext* opCtx,
                                                                       const Message& request,
                                                                       Date_t started) const {
    return ServiceEntryPointRouterRole::handleRequestImpl(opCtx, request, started);
}

}  // namespace mongo::txn_api::details

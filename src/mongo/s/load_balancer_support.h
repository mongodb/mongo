// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] load_balancer_support {

/**
 * Gets the load balancer port, if we are configured to enable one.
 */
boost::optional<int> getLoadBalancerPort();

/**
 * Helper for handling the `hello` command on mongos.
 * `helloHasLoadBalancedOption` must be true if the hello command had the
 * `loadBalanced` option present and set to `true`.
 *
 * For only the initial `hello` command, we respond to a `loadBalanced: true`
 * option by including the `serviceId` of this mongos in the hello reply.
 *
 * A connection marked as having come in through a load balancer must confirm
 * that it is using a load-balancer-aware driver by setting the `loadBalanced:
 * true` option in its first `hello` command. Otherwise, this function will
 * throw with `ErrorCodes::LoadBalancerSupportMismatch`.
 */
void handleHello(OperationContext* opCtx, BSONObjBuilder* result, bool helloHasLoadBalancedOption);

bool isLoadBalancerPeer(Client* client);

/**
 * Returns whether the feature flag for load balancer support is enabled.
 */
bool isEnabled();

/**
 * Returns the LSID of the session most recently used by the given Client as part of a
 * multi-statement transaction
 */
LogicalSessionId getMruSession(Client* client);

void setMruSession(Client* client, LogicalSessionId id);


}  // namespace load_balancer_support
}  // namespace mongo

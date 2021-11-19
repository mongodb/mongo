/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"

namespace mongo::load_balancer_support {

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

bool isFromLoadBalancer(Client* client);

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


}  // namespace mongo::load_balancer_support

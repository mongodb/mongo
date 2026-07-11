// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {
struct HostAndPort;
class ServiceContext;

namespace repl {

/**
 * An identifier unique to this instance. Used by isSelf to see if we are talking
 * to ourself or someone else.
 */
extern OID instanceId;

/**
 * Returns true if "hostAndPort" and "priorityPort" identify this instance. If no priorityPort
 * is specified then the checks on that field will be skipped.
 */
bool isSelf(const HostAndPort& hostAndPort,
            const boost::optional<int>& priorityPort,
            ServiceContext* ctx,
            Milliseconds timeout = Seconds(30));

/**
 * Returns true if "hostAndPort" and "priorityPort" identify this instance by checking our bound
 * IP addresses, without going out to the network and running the _isSelf command on the node.
 */
bool isSelfFastPath(const HostAndPort& hostAndPort, const boost::optional<int>& priorityPort);

/**
 * Returns true if "hostAndPort" and "priorityPort" identify this instance by running the _isSelf
 * command on the node. If the "priorityPort" is specified, will also run _isSelf against
 * host:priorityPort.
 */
bool isSelfSlowPath(const HostAndPort& hostAndPort,
                    const boost::optional<int>& priorityPort,
                    ServiceContext* ctx,
                    Milliseconds timeout);

/**
 * Returns all the IP addresses bound to the network interfaces of this machine.
 * This requires a syscall. If the ipv6enabled parameter is true, both IPv6 AND IPv4
 * addresses will be returned.
 *
 * Note: this only works on Linux and Windows. All calls should be properly ifdef'd,
 * otherwise an invariant will be triggered.
 */
std::vector<std::string> getBoundAddrs(bool ipv6enabled);

}  // namespace repl
}  // namespace mongo

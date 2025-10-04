/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace MONGO_MOD_PUB mongo {
struct HostAndPort;
class ServiceContext;

namespace repl {

/**
 * An identifier unique to this instance. Used by isSelf to see if we are talking
 * to ourself or someone else.
 */
extern OID instanceId;

/**
 * Returns true if "hostAndPort" identifies this instance.
 */
bool isSelf(const HostAndPort& hostAndPort,
            ServiceContext* ctx,
            Milliseconds timeout = Seconds(30));

/**
 * Returns true if "hostAndPort" identifies this instance by checking our bound IP addresses,
 * without going out to the network and running the _isSelf command on the node.
 */
bool isSelfFastPath(const HostAndPort& hostAndPort);

/**
 * Returns true if "hostAndPort" identifies this instance by running the _isSelf command on the
 * node.
 */
bool isSelfSlowPath(const HostAndPort& hostAndPort, ServiceContext* ctx, Milliseconds timeout);

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
}  // namespace MONGO_MOD_PUB mongo

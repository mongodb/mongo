/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/logv2/log_service.h"

namespace mongo {

/**
 * Represents the role this node plays in a sharded cluster, based on its startup arguments. Roles
 * are not mutually exclusive since a node can play different roles at the same time.
 *
 * Every node in a sharded cluster will have by default the RouterServer role. As a consequence, the
 * only possible combinations are:
 *  - { ShardServer, RouterServer }
 *  - { ShardServer, ConfigServer, RouterServer }
 *  - { RouterServer }
 * For a cluster that is not sharded, the cluster role of each node is { None }.
 */
class ClusterRole {
public:
    enum Value : uint8_t {
        /**
         * The node is not part of a sharded cluster.
         */
        None = 0x00,

        /**
         * The node acts as a shard server (the process was started with --shardsvr argument.) This
         * is implicitly set when the node is configured to act as a config server (the process was
         * started with --configsvr argument).
         */
        ShardServer = 0x01,

        /**
         * The node acts as a config server (the process was started with --configsvr argument).
         */
        ConfigServer = 0x02,

        /**
         * By default, all shard and config server nodes act as router servers.
         */
        RouterServer = 0x04
    };

    ClusterRole() = default;
    ClusterRole(Value role) : _roleMask{role} {}
    ClusterRole(std::initializer_list<Value> roles) {
        for (auto&& role : roles)
            _roleMask |= role;
        _checkRole();
    }

    ClusterRole& operator=(const ClusterRole& rhs) {
        if (this != &rhs) {
            _roleMask = rhs._roleMask;
            _checkRole();
        }
        return *this;
    }

    ClusterRole& operator+=(Value role) {
        _roleMask |= role;
        // TODO (SERVER-78810): Review these invariants as a node acting config and router roles (no
        // shard role) would be allowed.
        _checkRole();
        return *this;
    }

    /**
     * Returns `true` if this node plays the given role, `false` otherwise. Even if the node plays
     * the given role, it is not excluded that it also plays others.
     */
    bool has(const ClusterRole& role) const {
        return role._roleMask == None ? _roleMask == None : _roleMask & role._roleMask;
    }

    /**
     * Returns `true` if this node plays only the given role, `false` otherwise.
     */
    bool hasExclusively(const ClusterRole& role) const {
        return _roleMask == role._roleMask;
    }

    /**
     * Returns `true` if this node has the shard role and not the config role.
     */
    bool isShardOnly() const {
        return has(ShardServer) && !has(ConfigServer);
    }

private:
    void _checkRole() const;

    uint8_t _roleMask = 0;
};

/**
 * Returns a BSON array of strings representing each of the roles in `role`.
 */
BSONArray toBSON(ClusterRole role);

std::ostream& operator<<(std::ostream& os, ClusterRole r);

StringBuilder& operator<<(StringBuilder& s, ClusterRole r);

inline std::string toString(ClusterRole r) {
    std::ostringstream os;
    os << r;
    return os.str();
}

/**
 * Returns the LogService corresponding to `role`.
 * `role` must be None, ShardService, or RouterService.
 */
inline logv2::LogService toLogService(ClusterRole role) {
    if (role.hasExclusively(ClusterRole::ShardServer))
        return logv2::LogService::shard;
    if (role.hasExclusively(ClusterRole::RouterServer))
        return logv2::LogService::router;
    if (role.hasExclusively(ClusterRole::None))
        return logv2::LogService::none;
    MONGO_UNREACHABLE;
}

}  // namespace mongo

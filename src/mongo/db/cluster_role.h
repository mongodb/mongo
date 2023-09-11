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

namespace mongo {

/**
 * Represents the role this node plays in a sharded cluster, based on its startup arguments. Roles
 * are not mutually exclusive since a node can play different roles at the same time.
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
         * The node acts as a router server (the process was started with --routerPort argument).
         */
        RouterServer = 0x04
    };

    ClusterRole(Value role = ClusterRole::None);
    ClusterRole(std::initializer_list<Value> roles);
    ClusterRole& operator=(const ClusterRole& rhs);
    ClusterRole& operator+=(Value role);

    /**
     * Returns `true` if this node plays the given role, `false` otherwise. Even if the node plays
     * the given role, it is not excluded that it also plays others.
     */
    bool has(const ClusterRole& role) const;

    /**
     * Returns `true` if this node plays only the given role, `false` otherwise.
     */
    bool hasExclusively(const ClusterRole& role) const;

private:
    uint8_t _roleMask;
};

inline std::ostream& operator<<(std::ostream& os, ClusterRole r) {
    static const std::array<std::pair<ClusterRole, StringData>, 3> bitNames{{
        {ClusterRole::ShardServer, "shard"_sd},
        {ClusterRole::ConfigServer, "config"_sd},
        {ClusterRole::RouterServer, "router"_sd},
    }};

    StringData sep;
    os << "ClusterRole{";
    for (auto&& [key, name] : bitNames) {
        if (r.has(key)) {
            os << sep << name;
            sep = "|";
        }
    }
    os << "}";
    return os;
}

inline std::string toString(ClusterRole r) {
    std::ostringstream os;
    os << r;
    return os.str();
}

}  // namespace mongo

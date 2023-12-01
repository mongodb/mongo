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

#include "mongo/db/cluster_role.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

const std::array<std::pair<ClusterRole, StringData>, 3> roleNames{{
    {ClusterRole::ShardServer, "shard"_sd},
    {ClusterRole::ConfigServer, "config"_sd},
    {ClusterRole::RouterServer, "router"_sd},
}};

}  // namespace

ClusterRole::ClusterRole(Value role) : _roleMask(role) {}

ClusterRole::ClusterRole(std::initializer_list<Value> roles) : _roleMask(None) {
    for (const auto role : roles) {
        _roleMask |= role;
    }
    invariant(!hasExclusively(ClusterRole::ConfigServer),
              "Role cannot be set to config server only");
}

ClusterRole& ClusterRole::operator=(const ClusterRole& rhs) {
    if (this != &rhs) {
        _roleMask = rhs._roleMask;
    }
    invariant(!hasExclusively(ClusterRole::ConfigServer),
              "Role cannot be set to config server only");
    return *this;
}

ClusterRole& ClusterRole::operator+=(Value role) {
    _roleMask |= role;
    // TODO (SERVER-78810): Review these invariants as a node acting config and router roles (no
    // shard role) would be allowed.
    invariant(!hasExclusively(ClusterRole::ConfigServer),
              "Role cannot be set to config server only");
    return *this;
}

bool ClusterRole::has(const ClusterRole& role) const {
    return role._roleMask == None ? _roleMask == None : _roleMask & role._roleMask;
}

bool ClusterRole::hasExclusively(const ClusterRole& role) const {
    return _roleMask == role._roleMask;
}

logv2::LogService toLogService(ClusterRole role) {
    if (role.hasExclusively(ClusterRole::ShardServer))
        return logv2::LogService::shard;
    else if (role.hasExclusively(ClusterRole::RouterServer))
        return logv2::LogService::router;
    else if (role.hasExclusively(ClusterRole::None))
        return logv2::LogService::none;
    MONGO_UNREACHABLE;
}

BSONArray toBSON(ClusterRole role) {
    BSONArrayBuilder bab;
    for (auto&& [key, name] : roleNames) {
        if (role.has(key)) {
            bab.append(name);
        }
    }
    return bab.arr();
}

std::ostream& operator<<(std::ostream& os, ClusterRole r) {
    StringData sep;
    os << "ClusterRole{";
    for (auto&& [key, name] : roleNames) {
        if (r.has(key)) {
            os << sep << name;
            sep = "|";
        }
    }
    os << "}";
    return os;
}

StringBuilder& operator<<(StringBuilder& s, ClusterRole r) {
    StringData sep;
    s << "ClusterRole{";
    for (auto&& [key, name] : roleNames) {
        if (r.has(key)) {
            s << sep << name;
            sep = "|";
        }
    }
    s << "}";
    return s;
}

}  // namespace mongo

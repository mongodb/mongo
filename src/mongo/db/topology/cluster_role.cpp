// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_role.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

const std::array<std::pair<ClusterRole, std::string_view>, 3> roleNames{{
    {ClusterRole::ShardServer, "shard"sv},
    {ClusterRole::ConfigServer, "config"sv},
    {ClusterRole::RouterServer, "router"sv},
}};

}  // namespace

void ClusterRole::_checkRole() const {
    tassert(10555101,
            "Role cannot be set to config server only",
            !hasExclusively(ClusterRole::ConfigServer));
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
    std::string_view sep;
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
    std::string_view sep;
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

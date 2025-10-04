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

#include "mongo/db/topology/cluster_role.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <iostream>

namespace mongo {

namespace {

const std::array<std::pair<ClusterRole, StringData>, 3> roleNames{{
    {ClusterRole::ShardServer, "shard"_sd},
    {ClusterRole::ConfigServer, "config"_sd},
    {ClusterRole::RouterServer, "router"_sd},
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

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
#include "mongo/db/commands/server_status/server_status.h"

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace mongo {

namespace {
constexpr auto kTimingSection = "timing"_sd;
}  // namespace

ServerStatusSectionRegistry* ServerStatusSectionRegistry::instance() {
    static ServerStatusSectionRegistry instance;
    return &instance;
}

ServerStatusSectionRegistry::RoleTag ServerStatusSectionRegistry::getTagForRole(ClusterRole role) {
    if (role.hasExclusively(ClusterRole::ShardServer)) {
        return RoleTag::shard;
    }
    if (role.hasExclusively(ClusterRole::RouterServer)) {
        return RoleTag::router;
    }
    return RoleTag::shardAndRouter;
}

void ServerStatusSectionRegistry::addSection(std::unique_ptr<ServerStatusSection> section) {
    // Disallow adding a section named "timing" as it is reserved for the server status command.
    dassert(section->getSectionName() != kTimingSection);
    MONGO_verify(!_runCalled.load());
    const auto& name = section->getSectionName();
    const auto& role = section->getClusterRole();
    const auto roleTag = getTagForRole(role);

    // Before inserting, validate that no already-registered sections are incompatible with
    // `section`. Two sections are incompatible if they have the same name and RoleTag, or if they
    // have the same name and either has RoleTag::shardAndRouter.
    auto areRolesIncompatible = [](RoleTag r1, RoleTag r2) {
        return r1 == RoleTag::shardAndRouter || r2 == RoleTag::shardAndRouter || r1 == r2;
    };
    auto lower = _sections.lower_bound({name, RoleTag::shard});
    auto upper = _sections.upper_bound({name, RoleTag::shardAndRouter});
    for (auto&& it = lower; it != upper; ++it) {
        auto existingSectionRole = it->first.second;
        invariant(!areRolesIncompatible(existingSectionRole, roleTag),
                  fmt::format("Duplicate ServerStatusSection Registration with name {} and role {}",
                              name,
                              toString(role)));
    }
    auto [iter, ok] = _sections.try_emplace({name, roleTag}, std::move(section));
    invariant(ok,
              fmt::format("Duplicate ServerStatusSection Registration with name {} and role {}",
                          name,
                          toString(role)));
}

ServerStatusSectionRegistry::SectionMap::const_iterator ServerStatusSectionRegistry::begin() {
    _runCalled.store(true);
    return _sections.begin();
};

ServerStatusSectionRegistry::SectionMap::const_iterator ServerStatusSectionRegistry::end() {
    return _sections.end();
}

}  // namespace mongo

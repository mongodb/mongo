// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/commands/server_status/server_status.h"

#include "mongo/util/assert_util.h"

#include <string_view>

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
constexpr auto kTimingSection = "timing"sv;
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

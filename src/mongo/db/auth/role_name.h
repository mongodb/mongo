// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/auth_name.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

/**
 * Representation of a name of a role in a MongoDB system.
 *
 * Consists of a "role name"  part and a "datbase name" part.
 */
class RoleName : public AuthName<RoleName> {
public:
    static constexpr auto kName = "RoleName"sv;
    static constexpr auto kFieldName = "role"sv;

    using AuthName::AuthName;

    const std::string& getRole() const {
        return getName();
    }
};

using RoleNameIterator = AuthNameIterator<RoleName>;
template <typename Container>
using RoleNameContainerIteratorImpl = AuthNameContainerIteratorImpl<Container, RoleName>;

template <typename ContainerIterator>
RoleNameIterator makeRoleNameIterator(const ContainerIterator& begin,
                                      const ContainerIterator& end) {
    return RoleNameIterator(
        std::make_unique<RoleNameContainerIteratorImpl<ContainerIterator>>(begin, end));
}

template <typename Container>
RoleNameIterator makeRoleNameIteratorForContainer(const Container& container) {
    return makeRoleNameIterator(container.begin(), container.end());
}

template <typename Container>
Container roleNameIteratorToContainer(RoleNameIterator it) {
    Container container;
    while (it.more()) {
        container.emplace_back(it.next());
    }
    return container;
}

}  // namespace mongo

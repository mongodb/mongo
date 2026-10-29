// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/auth_name.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] UserName : public AuthName<UserName> {
public:
    static constexpr auto kName = "UserName"sv;
    static constexpr auto kFieldName = "user"sv;

    using AuthName::AuthName;

    const std::string& getUser() const {
        return getName();
    }
};

using UserNameIterator = AuthNameIterator<UserName>;
template <typename Container>
using UserNameContainerIteratorImpl = AuthNameContainerIteratorImpl<Container, UserName>;

template <typename ContainerIterator>
UserNameIterator makeUserNameIterator(const ContainerIterator& begin,
                                      const ContainerIterator& end) {
    return UserNameIterator(
        std::make_unique<UserNameContainerIteratorImpl<ContainerIterator>>(begin, end));
}

template <typename Container>
UserNameIterator makeUserNameIteratorForContainer(const Container& container) {
    return makeUserNameIterator(container.begin(), container.end());
}

template <typename Container>
Container userNameIteratorToContainer(UserNameIterator it) {
    Container container;
    while (it.more()) {
        container.emplace_back(it.next());
    }
    return container;
}

}  // namespace mongo

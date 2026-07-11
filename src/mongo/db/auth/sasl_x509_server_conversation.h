// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::auth {

class SaslX509ServerMechanism final : public MakeServerMechanism<X509Policy> {
public:
    explicit SaslX509ServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<X509Policy>(std::move(authenticationDatabase)) {}

    ~SaslX509ServerMechanism() override = default;

    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       std::string_view inputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return _step;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return kMaxStep;
    }

    bool isClusterMember(Client* client) const override;

    StatusWith<std::unique_ptr<UserRequest>> makeUserRequest(
        OperationContext* opCtx) const override;

private:
    static constexpr std::uint32_t kMaxStep = 1;

    std::uint32_t _step{0};
};

class X509ServerFactory : public MakeServerFactory<SaslX509ServerMechanism> {
public:
    using MakeServerFactory<SaslX509ServerMechanism>::MakeServerFactory;
    static constexpr bool isInternal = false;
    bool canMakeMechanismForUser(const User* user) const final {
        auto credentials = user->getCredentials();
        return credentials.isExternal;
    }
};

}  // namespace mongo::auth

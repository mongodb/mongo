// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class SASLPlainServerMechanism : public MakeServerMechanism<PLAINPolicy> {
public:
    explicit SASLPlainServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<PLAINPolicy>(std::move(authenticationDatabase)) {}

    boost::optional<std::uint32_t> currentStep() const override {
        return 1;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return 1;
    }

private:
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       std::string_view input) final;
};

class PLAINServerFactory : public MakeServerFactory<SASLPlainServerMechanism> {
public:
    using MakeServerFactory<SASLPlainServerMechanism>::MakeServerFactory;
    static constexpr bool isInternal = true;
    bool canMakeMechanismForUser(const User* user) const final {
        auto credentials = user->getCredentials();
        return !credentials.isExternal &&
            (credentials.scram<SHA1Block>().isValid() ||
             credentials.scram<SHA256Block>().isValid());
    }
};


}  // namespace mongo

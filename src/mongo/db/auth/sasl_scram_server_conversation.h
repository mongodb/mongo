// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/icu.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 *  Server side authentication session for SASL SCRAM-SHA-1/256.
 */
template <typename Policy>
class SaslSCRAMServerMechanism final : public MakeServerMechanism<Policy> {
public:
    using HashBlock = typename Policy::HashBlock;

    explicit SaslSCRAMServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<Policy>(std::move(authenticationDatabase)) {}

    /**
     * Take one step in a SCRAM-SHA-1 conversation.
     *
     * @return !Status::OK() if auth failed. The boolean part indicates if the
     * authentication conversation is finished or not.
     *
     **/
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       std::string_view inputData) override;

    StatusWith<std::string> saslPrep(std::string_view str) const {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return std::string{str};
        } else {
            return icuSaslPrep(str);
        }
    }

    Status setOptions(BSONObj options) final;

    boost::optional<std::uint32_t> currentStep() const override {
        return _step;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return _totalSteps();
    }

private:
    /**
     * Parse client-first-message and generate server-first-message
     **/
    StatusWith<std::tuple<bool, std::string>> _firstStep(OperationContext* opCtx,
                                                         std::string_view input);

    /**
     * Parse client-final-message and generate server-final-message
     **/
    StatusWith<std::tuple<bool, std::string>> _secondStep(OperationContext* opCtx,
                                                          std::string_view input);

    std::uint32_t _totalSteps() const {
        return _skipEmptyExchange ? 2 : 3;
    }

    std::uint32_t _step{0};
    std::string _authMessage;

    // The secrets to check the client proof against during the second step
    // Usually only contains one element, except during key rollover.
    std::vector<scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy>> _secrets;

    // client and server nonce concatenated
    std::string _nonce;

    // Do not send empty 3rd reply in scram conversation.
    bool _skipEmptyExchange{false};
};

extern template class SaslSCRAMServerMechanism<SCRAMSHA1Policy>;
extern template class SaslSCRAMServerMechanism<SCRAMSHA256Policy>;

template <typename ScramMechanism>
class SCRAMServerFactory : public MakeServerFactory<ScramMechanism> {
public:
    using MakeServerFactory<ScramMechanism>::MakeServerFactory;
    static constexpr bool isInternal = true;
    bool canMakeMechanismForUser(const User* user) const final {
        auto credentials = user->getCredentials();
        return credentials.scram<typename ScramMechanism::HashBlock>().isValid();
    }
};

using SaslSCRAMSHA1ServerMechanism = SaslSCRAMServerMechanism<SCRAMSHA1Policy>;
using SCRAMSHA1ServerFactory = SCRAMServerFactory<SaslSCRAMSHA1ServerMechanism>;

using SaslSCRAMSHA256ServerMechanism = SaslSCRAMServerMechanism<SCRAMSHA256Policy>;
using SCRAMSHA256ServerFactory = SCRAMServerFactory<SaslSCRAMSHA256ServerMechanism>;

}  // namespace mongo

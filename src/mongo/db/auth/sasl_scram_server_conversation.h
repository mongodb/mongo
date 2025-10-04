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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/icu.h"

#include <algorithm>
#include <cstring>
#include <string>
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
                                                       StringData inputData) override;

    StatusWith<std::string> saslPrep(StringData str) const {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return std::string{str};
        } else {
            return icuSaslPrep(str);
        }
    }

    Status setOptions(BSONObj options) final;

    boost::optional<unsigned int> currentStep() const override {
        return _step;
    }

    boost::optional<unsigned int> totalSteps() const override {
        return _totalSteps();
    }

private:
    /**
     * Parse client-first-message and generate server-first-message
     **/
    StatusWith<std::tuple<bool, std::string>> _firstStep(OperationContext* opCtx, StringData input);

    /**
     * Parse client-final-message and generate server-final-message
     **/
    StatusWith<std::tuple<bool, std::string>> _secondStep(OperationContext* opCtx,
                                                          StringData input);

    unsigned int _totalSteps() const {
        return _skipEmptyExchange ? 2 : 3;
    }

    unsigned int _step{0};
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

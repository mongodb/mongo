/*
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/util/icu.h"

namespace mongo {

/**
 *  Server side authentication session for SASL SCRAM-SHA-1.
 */
template <typename Policy>
class SaslSCRAMServerMechanism : public MakeServerMechanism<Policy> {
public:
    using HashBlock = typename Policy::HashBlock;

    explicit SaslSCRAMServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<Policy>(std::move(authenticationDatabase)) {}

    ~SaslSCRAMServerMechanism() final = default;

    /**
     * Take one step in a SCRAM-SHA-1 conversation.
     *
     * @return !Status::OK() if auth failed. The boolean part indicates if the
     * authentication conversation is finished or not.
     *
     **/
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData inputData);

    StatusWith<std::string> saslPrep(StringData str) const {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return str.toString();
        } else {
            return mongo::saslPrep(str);
        }
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

    int _step{0};
    std::string _authMessage;
    User::SCRAMCredentials<HashBlock> _scramCredentials;
    scram::Secrets<HashBlock> _secrets;

    // client and server nonce concatenated
    std::string _nonce;
};

extern template class SaslSCRAMServerMechanism<SCRAMSHA1Policy>;
extern template class SaslSCRAMServerMechanism<SCRAMSHA256Policy>;

template <typename ScramMechanism>
class SCRAMServerFactory : public MakeServerFactory<ScramMechanism> {
public:
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

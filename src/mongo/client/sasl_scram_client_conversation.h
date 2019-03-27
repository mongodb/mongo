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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/client/scram_client_cache.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/util/icu.h"

namespace mongo {

/**
 *  Client side authentication session for SASL PLAIN.
 */
class SaslSCRAMClientConversation : public SaslClientConversation {
    SaslSCRAMClientConversation(const SaslSCRAMClientConversation&) = delete;
    SaslSCRAMClientConversation& operator=(const SaslSCRAMClientConversation&) = delete;

public:
    using SaslClientConversation::SaslClientConversation;

    /**
     * Takes one step in a SCRAM conversation.
     *
     * @return !Status::OK() for failure. The boolean part indicates if the
     * authentication conversation is finished or not.
     *
     **/
    StatusWith<bool> step(StringData inputData, std::string* outputData) override;

    /**
     * Initialize the Presecrets/Secrets and return signed client proof.
     */
    virtual std::string generateClientProof(const std::vector<std::uint8_t>& salt,
                                            size_t iterationCount) = 0;

    /**
     * Verify the server's signature.
     */
    virtual bool verifyServerSignature(StringData sig) const = 0;

    /**
     * Runs saslPrep except on SHA-1.
     */
    virtual StatusWith<std::string> saslPrep(StringData val) const = 0;

private:
    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _firstStep(std::string* outputData);

    /**
     * Parses server-first-message and generate client-final-message.
     **/
    StatusWith<bool> _secondStep(StringData input, std::string* outputData);

    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _thirdStep(StringData input, std::string* outputData);

protected:
    int _step{0};
    std::string _authMessage;

    // client and server nonce concatenated
    std::string _clientNonce;
};

template <typename HashBlock>
class SaslSCRAMClientConversationImpl : public SaslSCRAMClientConversation {
public:
    SaslSCRAMClientConversationImpl(SaslClientSession* saslClientSession,
                                    SCRAMClientCache<HashBlock>* clientCache)
        : SaslSCRAMClientConversation(saslClientSession), _clientCache(clientCache) {}

    std::string generateClientProof(const std::vector<std::uint8_t>& salt,
                                    size_t iterationCount) final {
        auto password = uassertStatusOK(saslPrep(
            _saslClientSession->getParameter(SaslClientSession::parameterPassword).toString()));
        scram::Presecrets<HashBlock> presecrets(password, salt, iterationCount);

        auto targetHost = HostAndPort::parse(
            _saslClientSession->getParameter(SaslClientSession::parameterServiceHostAndPort));
        if (targetHost.isOK()) {
            _credentials = _clientCache->getCachedSecrets(targetHost.getValue(), presecrets);
            if (!_credentials) {
                _credentials = scram::Secrets<HashBlock>(presecrets);

                _clientCache->setCachedSecrets(
                    std::move(targetHost.getValue()), std::move(presecrets), _credentials);
            }
        } else {
            _credentials = scram::Secrets<HashBlock>(presecrets);
        }

        return _credentials.generateClientProof(_authMessage);
    }

    bool verifyServerSignature(StringData sig) const final {
        return _credentials.verifyServerSignature(_authMessage, sig);
    }

    StatusWith<std::string> saslPrep(StringData val) const final {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return val.toString();
        } else {
            return icuSaslPrep(val);
        }
    }

private:
    // Secrets and secrets cache
    scram::Secrets<HashBlock> _credentials;
    SCRAMClientCache<HashBlock>* const _clientCache;
};

using SaslSCRAMSHA1ClientConversation = SaslSCRAMClientConversationImpl<SHA1Block>;

}  // namespace mongo

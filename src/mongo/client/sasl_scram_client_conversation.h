// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/client/scram_client_cache.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/icu.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
    StatusWith<bool> step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return _step;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return _maxStep;
    }

    /**
     * Initialize the Presecrets/Secrets and return signed client proof.
     */
    virtual std::string generateClientProof(const std::vector<std::uint8_t>& salt,
                                            size_t iterationCount) = 0;

    /**
     * Verify the server's signature.
     */
    virtual bool verifyServerSignature(std::string_view sig) const = 0;

    /**
     * Runs saslPrep except on SHA-1.
     */
    virtual StatusWith<std::string> saslPrep(std::string_view val) const = 0;

private:
    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _firstStep(std::string* outputData);

    /**
     * Parses server-first-message and generate client-final-message.
     **/
    StatusWith<bool> _secondStep(std::string_view input, std::string* outputData);

    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _thirdStep(std::string_view input, std::string* outputData);

protected:
    std::uint32_t _step{0};
    const std::uint32_t _maxStep = 3;
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
            std::string{_saslClientSession->getParameter(SaslClientSession::parameterPassword)}));
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

    bool verifyServerSignature(std::string_view sig) const final {
        return _credentials.verifyServerSignature(_authMessage, sig);
    }

    StatusWith<std::string> saslPrep(std::string_view val) const final {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return std::string{val};
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

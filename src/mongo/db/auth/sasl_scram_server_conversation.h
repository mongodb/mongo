/*
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/sasl_server_conversation.h"
#include "mongo/util/icu.h"

namespace mongo {
/**
 *  Server side authentication session for SASL SCRAM-SHA-1.
 */
class SaslSCRAMServerConversation : public SaslServerConversation {
    MONGO_DISALLOW_COPYING(SaslSCRAMServerConversation);

public:
    explicit SaslSCRAMServerConversation(SaslAuthenticationSession* session)
        : SaslServerConversation(session) {}
    ~SaslSCRAMServerConversation() override = default;

    /**
     * Take one step in a SCRAM-SHA-1 conversation.
     *
     * @return !Status::OK() if auth failed. The boolean part indicates if the
     * authentication conversation is finished or not.
     *
     **/
    StatusWith<bool> step(StringData inputData, std::string* outputData) override;

    /**
     * Initialize details, called after _creds has been loaded.
     */
    virtual bool initAndValidateCredentials() = 0;

    /**
     * Provide the predetermined salt to the client.
     */
    virtual std::string getSalt() const = 0;

    /**
     * Provide the predetermined iteration count to the client.
     */
    virtual size_t getIterationCount() const = 0;

    /**
     * Verify proof submitted by authenticating client.
     */
    virtual bool verifyClientProof(StringData) const = 0;

    /**
     * Generate a signature to prove ourselves.
     */
    virtual std::string generateServerSignature() const = 0;

    /**
     * Runs saslPrep except on SHA-1.
     */
    virtual StatusWith<std::string> saslPrep(StringData str) const = 0;

private:
    /**
     * Parse client-first-message and generate server-first-message
     **/
    StatusWith<bool> _firstStep(std::vector<std::string>& input, std::string* outputData);

    /**
     * Parse client-final-message and generate server-final-message
     **/
    StatusWith<bool> _secondStep(const std::vector<std::string>& input, std::string* outputData);

protected:
    int _step{0};
    std::string _authMessage;
    User::CredentialData _creds;

    // client and server nonce concatenated
    std::string _nonce;
};

template <typename HashBlock>
class SaslSCRAMServerConversationImpl : public SaslSCRAMServerConversation {
public:
    explicit SaslSCRAMServerConversationImpl(SaslAuthenticationSession* session)
        : SaslSCRAMServerConversation(session) {}
    ~SaslSCRAMServerConversationImpl() override = default;

    bool initAndValidateCredentials() final {
        const auto& scram = _creds.scram<HashBlock>();
        if (!scram.isValid()) {
            return false;
        }
        if (!_credentials) {
            _credentials = scram::Secrets<HashBlock>(
                "", base64::decode(scram.storedKey), base64::decode(scram.serverKey));
        }
        return true;
    }

    std::string getSalt() const final {
        return _creds.scram<HashBlock>().salt;
    }

    size_t getIterationCount() const final {
        return _creds.scram<HashBlock>().iterationCount;
    }

    bool verifyClientProof(StringData clientProof) const final {
        return _credentials.verifyClientProof(_authMessage, clientProof);
    }

    std::string generateServerSignature() const final {
        return _credentials.generateServerSignature(_authMessage);
    }

    StatusWith<std::string> saslPrep(StringData str) const final {
        if (std::is_same<SHA1Block, HashBlock>::value) {
            return str.toString();
        } else {
            return mongo::saslPrep(str);
        }
    }

private:
    scram::Secrets<HashBlock> _credentials;
};

using SaslSCRAMSHA1ServerConversation = SaslSCRAMServerConversationImpl<SHA1Block>;

}  // namespace mongo

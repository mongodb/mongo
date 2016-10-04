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
#include "mongo/base/string_data.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/crypto/mechanism_scram.h"

namespace mongo {
/**
 *  Client side authentication session for SASL PLAIN.
 */
class SaslSCRAMSHA1ClientConversation : public SaslClientConversation {
    MONGO_DISALLOW_COPYING(SaslSCRAMSHA1ClientConversation);

public:
    /**
     * Implements the client side of a SASL PLAIN mechanism session.
     **/
    explicit SaslSCRAMSHA1ClientConversation(SaslClientSession* saslClientSession);

    virtual ~SaslSCRAMSHA1ClientConversation();

    /**
     * Takes one step in a SCRAM-SHA-1 conversation.
     *
     * @return !Status::OK() for failure. The boolean part indicates if the
     * authentication conversation is finished or not.
     *
     **/
    virtual StatusWith<bool> step(StringData inputData, std::string* outputData);

private:
    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _firstStep(std::string* outputData);

    /**
     * Parses server-first-message and generate client-final-message.
     **/
    StatusWith<bool> _secondStep(const std::vector<std::string>& input, std::string* outputData);

    /**
     * Generates client-first-message.
     **/
    StatusWith<bool> _thirdStep(const std::vector<std::string>& input, std::string* outputData);

    int _step;
    std::string _authMessage;
    unsigned char _saltedPassword[scram::hashSize];

    // client and server nonce concatenated
    std::string _clientNonce;
};

}  // namespace mongo

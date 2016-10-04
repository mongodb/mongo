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

#include "mongo/platform/basic.h"

#include "mongo/client/sasl_scramsha1_client_conversation.h"

#include <boost/algorithm/string/replace.hpp>

#include "mongo/base/parse_number.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/text.h"

namespace mongo {

using std::unique_ptr;
using std::string;

SaslSCRAMSHA1ClientConversation::SaslSCRAMSHA1ClientConversation(
    SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession), _step(0), _authMessage(""), _clientNonce("") {}

SaslSCRAMSHA1ClientConversation::~SaslSCRAMSHA1ClientConversation() {
    // clear the _saltedPassword memory
    memset(_saltedPassword, 0, scram::hashSize);
}

StatusWith<bool> SaslSCRAMSHA1ClientConversation::step(StringData inputData,
                                                       std::string* outputData) {
    std::vector<std::string> input = StringSplitter::split(inputData.toString(), ",");
    _step++;

    switch (_step) {
        case 1:
            return _firstStep(outputData);
        case 2:
            // Append server-first-message to _authMessage
            _authMessage += inputData.toString() + ",";
            return _secondStep(input, outputData);
        case 3:
            return _thirdStep(input, outputData);
        default:
            return StatusWith<bool>(
                ErrorCodes::AuthenticationFailed,
                mongoutils::str::stream() << "Invalid SCRAM-SHA-1 authentication step: " << _step);
    }
}

/*
 * RFC 5802 specifies that in SCRAM user names characters ',' and '=' are encoded as
 * =2C and =3D respectively.
 */
static void encodeSCRAMUsername(std::string& user) {
    boost::replace_all(user, "=", "=3D");
    boost::replace_all(user, ",", "=2C");
}

/*
 * Generate client-first-message of the form:
 * n,a=authzid,n=encoded-username,r=client-nonce
 */
StatusWith<bool> SaslSCRAMSHA1ClientConversation::_firstStep(std::string* outputData) {
    if (_saslClientSession->getParameter(SaslClientSession::parameterPassword).empty()) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Empty client password provided");
    }

    // Create text-based nonce as base64 encoding of a binary blob of length multiple of 3
    const int nonceLenQWords = 3;
    uint64_t binaryNonce[nonceLenQWords];

    unique_ptr<SecureRandom> sr(SecureRandom::create());

    binaryNonce[0] = sr->nextInt64();
    binaryNonce[1] = sr->nextInt64();
    binaryNonce[2] = sr->nextInt64();

    std::string user =
        _saslClientSession->getParameter(SaslClientSession::parameterUser).toString();
    encodeSCRAMUsername(user);
    std::string clientNonce =
        base64::encode(reinterpret_cast<char*>(binaryNonce), sizeof(binaryNonce));

    // Append client-first-message-bare to authMessage
    _authMessage = "n=" + user + ",r=" + clientNonce + ",";

    StringBuilder sb;
    sb << "n,,n=" << user << ",r=" << clientNonce;
    *outputData = sb.str();

    return StatusWith<bool>(false);
}

/**
 * Parse server-first-message on the form:
 * r=client-nonce|server-nonce,s=user-salt,i=iteration-count
 *
 * Generate client-final-message of the form:
 * c=channel-binding(base64),r=client-nonce|server-nonce,p=ClientProof
 *
 **/
StatusWith<bool> SaslSCRAMSHA1ClientConversation::_secondStep(const std::vector<string>& input,
                                                              std::string* outputData) {
    if (input.size() != 3) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Incorrect number of arguments for first SCRAM-SHA-1 server message, got "
                << input.size()
                << " expected 3");
    } else if (!str::startsWith(input[0], "r=") || input[0].size() < 2) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream() << "Incorrect SCRAM-SHA-1 client|server nonce: " << input[0]);
    } else if (!str::startsWith(input[1], "s=") || input[1].size() < 6) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream() << "Incorrect SCRAM-SHA-1 salt: "
                                                          << input[1]);
    } else if (!str::startsWith(input[2], "i=") || input[2].size() < 3) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream() << "Incorrect SCRAM-SHA-1 iteration count: " << input[2]);
    }

    std::string nonce = input[0].substr(2);
    if (!str::startsWith(nonce, _clientNonce)) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream()
                                    << "Server SCRAM-SHA-1 nonce does not match client nonce"
                                    << input[2]);
    }

    std::string salt = input[1].substr(2);
    int iterationCount;

    Status status = parseNumberFromStringWithBase(input[2].substr(2), 10, &iterationCount);
    if (status != Status::OK()) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream()
                                    << "Failed to parse SCRAM-SHA-1 iteration count: "
                                    << input[2]);
    }

    // Append client-final-message-without-proof to _authMessage
    _authMessage += "c=biws,r=" + nonce;

    std::string decodedSalt;
    try {
        decodedSalt = base64::decode(salt);
    } catch (const DBException& ex) {
        return StatusWith<bool>(ex.toStatus());
    }

    scram::generateSaltedPassword(
        _saslClientSession->getParameter(SaslClientSession::parameterPassword),
        reinterpret_cast<const unsigned char*>(decodedSalt.c_str()),
        decodedSalt.size(),
        iterationCount,
        _saltedPassword);

    std::string clientProof = scram::generateClientProof(_saltedPassword, _authMessage);

    StringBuilder sb;
    sb << "c=biws,r=" << nonce << ",p=" << clientProof;
    *outputData = sb.str();

    return StatusWith<bool>(false);
}

/**
 * Verify server-final-message on the form:
 * v=ServerSignature
 *
 * or failed authentication server-final-message on the form:
 * e=message
 **/
StatusWith<bool> SaslSCRAMSHA1ClientConversation::_thirdStep(const std::vector<string>& input,
                                                             std::string* outputData) {
    if (input.size() != 1) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Incorrect number of arguments for final SCRAM-SHA-1 server message, got "
                << input.size()
                << " expected 1");
    } else if (input[0].size() < 3) {
        return StatusWith<bool>(ErrorCodes::BadValue,
                                mongoutils::str::stream()
                                    << "Incorrect SCRAM-SHA-1 server message length: "
                                    << input[0]);
    } else if (str::startsWith(input[0], "e=")) {
        return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                mongoutils::str::stream() << "SCRAM-SHA-1 authentication failure: "
                                                          << input[0].substr(2));
    } else if (!str::startsWith(input[0], "v=")) {
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream() << "Incorrect SCRAM-SHA-1 ServerSignature: " << input[0]);
    }

    bool validServerSignature =
        scram::verifyServerSignature(_saltedPassword, _authMessage, input[0].substr(2));

    if (!validServerSignature) {
        *outputData = "e=Invalid server signature";
        return StatusWith<bool>(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Client failed to verify SCRAM-SHA-1 ServerSignature, received "
                << input[0].substr(2));
    }

    *outputData = "";

    return StatusWith<bool>(true);
}
}  // namespace mongo

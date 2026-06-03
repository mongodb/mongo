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

#include "mongo/client/sasl_scram_client_conversation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <deque>
#include <memory>

#include <absl/strings/str_split.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo {

using std::string;

StatusWith<bool> SaslSCRAMClientConversation::step(StringData inputData, std::string* outputData) {
    _step++;

    switch (_step) {
        case 1:
            return _firstStep(outputData);
        case 2:
            return _secondStep(inputData, outputData);
        case 3:
            return _thirdStep(inputData, outputData);
        default:
            return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                                    str::stream()
                                        << "Invalid SCRAM authentication step: " << _step);
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
StatusWith<bool> SaslSCRAMClientConversation::_firstStep(std::string* outputData) {
    if (_saslClientSession->getParameter(SaslClientSession::parameterPassword).empty()) {
        return Status(ErrorCodes::BadValue, "Empty client password provided");
    }

    // Create text-based nonce as base64 encoding of a binary blob of length multiple of 3
    static constexpr size_t nonceLenQWords = 3;
    uint64_t binaryNonce[nonceLenQWords];

    SecureRandom().fill(binaryNonce, sizeof(binaryNonce));

    std::string user =
        std::string{_saslClientSession->getParameter(SaslClientSession::parameterUser)};

    encodeSCRAMUsername(user);
    _clientNonce =
        base64::encode(StringData(reinterpret_cast<char*>(binaryNonce), sizeof(binaryNonce)));

    // Append client-first-message-bare to authMessage
    _authMessage = "n=" + user + ",r=" + _clientNonce;

    StringBuilder sb;
    sb << "n,," << _authMessage;
    *outputData = sb.str();

    return false;
}

/**
 * Parse server-first-message on the form:
 * [reserved-mext ',']r=client-nonce|server-nonce,s=user-salt,i=iteration-count[,extensions]
 *
 * Generate client-final-message of the form:
 * c=channel-binding(base64),r=client-nonce|server-nonce,p=ClientProof
 *
 **/
StatusWith<bool> SaslSCRAMClientConversation::_secondStep(StringData inputData,
                                                          std::string* outputData) {
    if (inputData.starts_with("m=")) {
        return Status(ErrorCodes::BadValue, "SCRAM required extensions not supported");
    }
    const std::vector<std::string> input =
        absl::StrSplit(toStdStringViewForInterop(inputData), ",", absl::SkipEmpty());

    if (input.size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Incorrect number of arguments for first SCRAM server message, got "
                          << input.size() << " expected at least 3");
    }

    if (!str::startsWith(input[0], "r=") || input[0].size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM client|server nonce: " << input[0]);
    }

    const auto nonce = input[0].substr(2);
    if (!str::startsWith(nonce, _clientNonce)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Server SCRAM nonce does not match client nonce: " << nonce);
    }

    if (!str::startsWith(input[1], "s=") || input[1].size() < 6) {
        return Status(ErrorCodes::BadValue, str::stream() << "Incorrect SCRAM salt: " << input[1]);
    }
    const auto salt64 = input[1].substr(2);

    if (!str::startsWith(input[2], "i=") || input[2].size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM iteration count: " << input[2]);
    }
    size_t iterationCount;
    Status status = NumberParser().base(10)(input[2].substr(2), &iterationCount);
    if (!status.isOK()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Failed to parse SCRAM iteration count: " << input[2]);
    }

    // Append server-first-message and client-final-message-without-proof.
    _authMessage += "," + std::string{inputData} + ",c=biws,r=" + nonce;

    std::string decodedSalt, clientProof;
    try {
        decodedSalt = base64::decode(salt64);
        clientProof = generateClientProof(
            std::vector<std::uint8_t>(decodedSalt.begin(), decodedSalt.end()), iterationCount);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    StringBuilder sb;
    sb << "c=biws,r=" << nonce << ",p=" << clientProof;
    *outputData = sb.str();

    return false;
}

/**
 * Verify server-final-message on the form:
 * v=ServerSignature
 *
 * or failed authentication server-final-message on the form:
 * e=message
 **/
StatusWith<bool> SaslSCRAMClientConversation::_thirdStep(StringData inputData,
                                                         std::string* outputData) {
    const std::vector<std::string> input =
        absl::StrSplit(toStdStringViewForInterop(inputData), ",", absl::SkipEmpty());

    if (input.empty()) {
        return Status(
            ErrorCodes::BadValue,
            "Incorrect number of arguments for final SCRAM server message, got 0 expected 1");
    }

    if (input[0].size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM server message length: " << input[0]);
    }

    if (str::startsWith(input[0], "e=")) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "SCRAM authentication failure: " << input[0].substr(2));
    }

    if (!str::startsWith(input[0], "v=")) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM ServerSignature: " << input[0]);
    }

    if (!verifyServerSignature(base64::decode(input[0].substr(2)))) {
        *outputData = "e=Invalid server signature";
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Client failed to verify SCRAM ServerSignature, received "
                                    << input[0].substr(2));
    }

    *outputData = "";

    return true;
}
}  // namespace mongo

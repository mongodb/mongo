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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/sasl_scram_server_conversation.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

namespace mongo {


template <typename Policy>
StatusWith<std::tuple<bool, std::string>> SaslSCRAMServerMechanism<Policy>::stepImpl(
    OperationContext* opCtx, StringData inputData) {
    _step++;

    if (_step > 3 || _step <= 0) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Invalid SCRAM authentication step: " << _step);
    }

    if (_step == 1) {
        return _firstStep(opCtx, inputData);
    }
    if (_step == 2) {
        return _secondStep(opCtx, inputData);
    }

    return std::make_tuple(true, std::string{});
}

/*
 * RFC 5802 specifies that in SCRAM user names characters ',' and '=' are encoded as
 * =2C and =3D respectively.
 */
static void decodeSCRAMUsername(std::string& user) {
    boost::replace_all(user, "=2C", ",");
    boost::replace_all(user, "=3D", "=");
}

/*
 * Parse client-first-message of the form:
 * n,a=authzid,n=encoded-username,r=client-nonce
 *
 * Generate server-first-message on the form:
 * r=client-nonce|server-nonce,s=user-salt,i=iteration-count
 *
 * NOTE: we are ignoring the authorization ID part of the message
 */
template <typename Policy>
StatusWith<std::tuple<bool, std::string>> SaslSCRAMServerMechanism<Policy>::_firstStep(
    OperationContext* opCtx, StringData inputData) {
    const auto badCount = [](int got) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Incorrect number of arguments for first SCRAM client message, got "
                          << got << " expected at least 3");
    };

    /**
     * gs2-cbind-flag := ("p=" cb-name) / 'y' / 'n'
     * gs2-header := gs2-cbind-flag ',' [ authzid ] ','
     * reserved-mext := "m=" 1*(value-char)
     * client-first-message-bare := [reserved-mext  ','] username ',' nonce [',' extensions]
     * client-first-message := gs2-header client-first-message-bare
     */
    const auto gs2_cbind_comma = inputData.find(',');
    if (gs2_cbind_comma == std::string::npos) {
        return badCount(1);
    }
    const auto gs2_cbind_flag = inputData.substr(0, gs2_cbind_comma);
    if (gs2_cbind_flag.startsWith("p=")) {
        return Status(ErrorCodes::BadValue, "Server does not support channel binding");
    }

    if ((gs2_cbind_flag != "y") && (gs2_cbind_flag != "n")) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM client message prefix: " << gs2_cbind_flag);
    }

    const auto gs2_header_comma = inputData.find(',', gs2_cbind_comma + 1);
    if (gs2_header_comma == std::string::npos) {
        return badCount(2);
    }
    auto authzId = inputData.substr(gs2_cbind_comma + 1, gs2_header_comma - (gs2_cbind_comma + 1));
    if (authzId.size()) {
        if (authzId.startsWith("a=")) {
            authzId = authzId.substr(2);
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Incorrect SCRAM authzid: " << authzId);
        }
    }

    const auto client_first_message_bare = inputData.substr(gs2_header_comma + 1);
    if (client_first_message_bare.startsWith("m=")) {
        return Status(ErrorCodes::BadValue, "SCRAM mandatory extensions are not supported");
    }

    /* StringSplitter::split() will ignore consecutive delimiters.
     * e.g. "foo,,bar" => {"foo","bar"}
     * This makes our implementation of SCRAM *slightly* more generous
     * in what it will accept than the standard calls for.
     *
     * This does not impact _authMessage, as it's composed from the raw
     * string input, rather than the output of the split operation.
     */
    const auto input = StringSplitter::split(client_first_message_bare.toString(), ",");

    if (input.size() < 2) {
        // gs2-header is not included in this count, so add it back in.
        return badCount(input.size() + 2);
    }

    if (!str::startsWith(input[0], "n=") || input[0].size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid SCRAM user name: " << input[0]);
    }
    ServerMechanismBase::_principalName = input[0].substr(2);
    decodeSCRAMUsername(ServerMechanismBase::_principalName);

    if (!authzId.empty() && ServerMechanismBase::_principalName != authzId) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "SCRAM user name " << ServerMechanismBase::_principalName
                                    << " does not match authzid " << authzId);
    }

    if (!str::startsWith(input[1], "r=") || input[1].size() < 6) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid SCRAM client nonce: " << input[1]);
    }
    const auto clientNonce = input[1].substr(2);

    UserName user(ServerMechanismBase::ServerMechanismBase::_principalName,
                  ServerMechanismBase::getAuthenticationDatabase());

    // SERVER-16534, some mechanisms must be enabled for authenticating the internal user, so that
    // cluster members may communicate with each other. Hence ignore disabled auth mechanism
    // for the internal user.
    if (Policy::isInternalAuthMech() &&
        !sequenceContains(saslGlobalParams.authenticationMechanisms, Policy::getName()) &&
        user != internalSecurity.user->getName()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << Policy::getName() << " authentication is disabled");
    }

    // The authentication database is also the source database for the user.
    auto authManager = AuthorizationManager::get(opCtx->getServiceContext());

    auto swUser = authManager->acquireUser(opCtx, user);
    if (!swUser.isOK()) {
        return swUser.getStatus();
    }
    auto userObj = std::move(swUser.getValue());

    User::CredentialData credentials = userObj->getCredentials();
    UserName userName = userObj->getName();

    auto scramCredentials = credentials.scram<HashBlock>();

    if (!scramCredentials.isValid()) {
        // Check for authentication attempts of the __system user on
        // systems started without a keyfile.
        if (userName == internalSecurity.user->getName()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          "It is not possible to authenticate as the __system user "
                          "on servers started without a --keyFile parameter");
        } else {
            return Status(ErrorCodes::AuthenticationFailed,
                          "Unable to perform SCRAM authentication for a user with missing "
                          "or invalid SCRAM credentials");
        }
    }

    _secrets.push_back(scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy>(
        "",
        base64::decode(scramCredentials.storedKey),
        base64::decode(scramCredentials.serverKey)));

    if (userName == internalSecurity.user->getName() && internalSecurity.alternateCredentials) {
        auto altCredentials = internalSecurity.alternateCredentials->scram<HashBlock>();
        _secrets.push_back(scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy>(
            "",
            base64::decode(altCredentials.storedKey),
            base64::decode(altCredentials.serverKey)));
    }

    // Generate server-first-message
    // Create text-based nonce as base64 encoding of a binary blob of length multiple of 3
    const int nonceLenQWords = 3;
    uint64_t binaryNonce[nonceLenQWords];

    SecureRandom().fill(binaryNonce, sizeof(binaryNonce));

    _nonce =
        clientNonce + base64::encode(reinterpret_cast<char*>(binaryNonce), sizeof(binaryNonce));
    StringBuilder sb;
    sb << "r=" << _nonce << ",s=" << scramCredentials.salt
       << ",i=" << scramCredentials.iterationCount;
    std::string outputData = sb.str();

    // add client-first-message-bare and server-first-message to _authMessage
    _authMessage = client_first_message_bare.toString() + "," + outputData;

    return std::make_tuple(false, std::move(outputData));
}

/**
 * Parse client-final-message of the form:
 * c=channel-binding(base64),r=client-nonce|server-nonce,p=ClientProof
 *
 * Generate successful authentication server-final-message on the form:
 * v=ServerSignature
 *
 * or failed authentication server-final-message on the form:
 * e=message
 *
 * NOTE: we are ignoring the channel binding part of the message
 **/
template <typename Policy>
StatusWith<std::tuple<bool, std::string>> SaslSCRAMServerMechanism<Policy>::_secondStep(
    OperationContext* opCtx, StringData inputData) {
    const auto badCount = [](int got) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Incorrect number of arguments for second SCRAM client message, got "
                          << got << " expected at least 3");
    };

    /**
     * client-final-message-without-proof := cbind ',' nonce ',' [ ',' extensions ]
     * client-final-message := client-final-message-without-proof ',' proof
     */
    const auto last_comma = inputData.rfind(',');
    if (last_comma == std::string::npos) {
        return badCount(1);
    }

    // add client-final-message-without-proof to authMessage
    const auto client_final_message_without_proof = inputData.substr(0, last_comma);
    _authMessage += "," + client_final_message_without_proof.toString();

    const auto last_field = inputData.substr(last_comma + 1);
    if ((last_field.size() < 3) || !last_field.startsWith("p=")) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM ClientProof: " << last_field);
    }
    const auto proof = last_field.substr(2);

    const auto input = StringSplitter::split(client_final_message_without_proof.toString(), ",");
    if (input.size() < 2) {
        // Add count for proof back on.
        return badCount(input.size() + 1);
    }

    if (!str::startsWith(input[0], "c=") || input[0].size() < 3) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM channel binding: " << input[0]);
    }
    const auto cbind = input[0].substr(2);

    if (!str::startsWith(input[1], "r=") || input[1].size() < 6) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Incorrect SCRAM client|server nonce: " << input[1]);
    }
    const auto nonce = input[1].substr(2);

    // Concatenated nonce sent by client should equal the one in server-first-message
    if (nonce != _nonce) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Unmatched SCRAM nonce received from client in second step, expected "
                          << _nonce << " but received " << nonce);
    }

    // Do server side computations, compare storedKeys and generate client-final-message
    // AuthMessage     := client-first-message-bare + "," +
    //                    server-first-message + "," +
    //                    client-final-message-without-proof
    // ClientSignature := HMAC(StoredKey, AuthMessage)
    // ClientKey := ClientSignature XOR ClientProof
    // ServerSignature := HMAC(ServerKey, AuthMessage)

    const auto decodedProof = base64::decode(proof.toString());
    std::string serverSignature;
    const auto checkSecret =
        [&](const scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy>& secret) {
            if (!secret.verifyClientProof(_authMessage, decodedProof))
                return false;

            serverSignature = secret.generateServerSignature(_authMessage);
            return true;
        };

    if (!std::any_of(_secrets.begin(), _secrets.end(), checkSecret)) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "SCRAM authentication failed, storedKey mismatch");
    }

    invariant(!serverSignature.empty());
    StringBuilder sb;
    // ServerSignature := HMAC(ServerKey, AuthMessage)
    sb << "v=" << serverSignature;

    return std::make_tuple(false, sb.str());
}

template class SaslSCRAMServerMechanism<SCRAMSHA1Policy>;
template class SaslSCRAMServerMechanism<SCRAMSHA256Policy>;

namespace {
GlobalSASLMechanismRegisterer<SCRAMSHA1ServerFactory> scramsha1Registerer;
GlobalSASLMechanismRegisterer<SCRAMSHA256ServerFactory> scramsha256Registerer;
}  // namespace
}  // namespace mongo

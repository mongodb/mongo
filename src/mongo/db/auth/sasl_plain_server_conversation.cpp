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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/auth/sasl_plain_server_conversation.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"
#include "mongo/util/base64.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {
template <typename HashBlock>
StatusWith<bool> trySCRAM(const User::CredentialData& credentials, StringData pwd) {
    const auto scram = credentials.scram<HashBlock>();
    if (!scram.isValid()) {
        // No stored credentials available.
        return false;
    }

    const auto decodedSalt = base64::decode(scram.salt);
    scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy> secrets(scram::Presecrets<HashBlock>(
        pwd.toString(),
        std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(decodedSalt.c_str()),
                                  reinterpret_cast<const std::uint8_t*>(decodedSalt.c_str()) +
                                      decodedSalt.size()),
        scram.iterationCount));
    if (scram.storedKey !=
        base64::encode(reinterpret_cast<const char*>(secrets.storedKey().data()),
                       secrets.storedKey().size())) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Incorrect user name or password");
    }

    return true;
}
}  // namespace

StatusWith<std::tuple<bool, std::string>> SASLPlainServerMechanism::stepImpl(
    OperationContext* opCtx, StringData inputData) {
    if (_authenticationDatabase == "$external") {
        return Status(ErrorCodes::AuthenticationFailed,
                      "PLAIN mechanism must be used with internal users");
    }

    AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getServiceContext());

    // Expecting user input on the form: [authz-id]\0authn-id\0pwd
    std::string input = inputData.toString();

    SecureAllocatorAuthDomain::SecureString pwd = "";
    try {
        size_t firstNull = inputData.find('\0');
        if (firstNull == std::string::npos) {
            return Status(
                ErrorCodes::AuthenticationFailed,
                str::stream()
                    << "Incorrectly formatted PLAIN client message, missing first NULL delimiter");
        }
        size_t secondNull = inputData.find('\0', firstNull + 1);
        if (secondNull == std::string::npos) {
            return Status(
                ErrorCodes::AuthenticationFailed,
                str::stream()
                    << "Incorrectly formatted PLAIN client message, missing second NULL delimiter");
        }

        std::string authorizationIdentity = input.substr(0, firstNull);
        ServerMechanismBase::_principalName =
            input.substr(firstNull + 1, (secondNull - firstNull) - 1);
        if (ServerMechanismBase::_principalName.empty()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          str::stream()
                              << "Incorrectly formatted PLAIN client message, empty username");
        } else if (!authorizationIdentity.empty() &&
                   authorizationIdentity != ServerMechanismBase::_principalName) {
            return Status(ErrorCodes::AuthenticationFailed,
                          str::stream()
                              << "SASL authorization identity must match authentication identity");
        }
        pwd = SecureAllocatorAuthDomain::SecureString(input.substr(secondNull + 1).c_str());
        if (pwd->empty()) {
            return Status(ErrorCodes::AuthenticationFailed,
                          str::stream()
                              << "Incorrectly formatted PLAIN client message, empty password");
        }
    } catch (std::out_of_range&) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Incorrectly formatted PLAIN client message");
    }

    // The authentication database is also the source database for the user.
    auto swUser = authManager->acquireUser(
        opCtx, UserName(ServerMechanismBase::_principalName, _authenticationDatabase));

    if (!swUser.isOK()) {
        return swUser.getStatus();
    }

    auto userObj = std::move(swUser.getValue());
    const auto creds = userObj->getCredentials();

    const auto sha256Status = trySCRAM<SHA256Block>(creds, pwd->c_str());
    if (!sha256Status.isOK()) {
        return sha256Status.getStatus();
    }
    if (sha256Status.getValue()) {
        return std::make_tuple(true, std::string());
    }

    const auto authDigest = createPasswordDigest(ServerMechanismBase::_principalName, pwd->c_str());
    const auto sha1Status = trySCRAM<SHA1Block>(creds, authDigest);
    if (!sha1Status.isOK()) {
        return sha1Status.getStatus();
    }
    if (sha1Status.getValue()) {
        return std::make_tuple(true, std::string());
    }

    return Status(ErrorCodes::AuthenticationFailed, str::stream() << "No credentials available.");


    return std::make_tuple(true, std::string());
}

namespace {
GlobalSASLMechanismRegisterer<PLAINServerFactory> plainRegisterer;
}  // namespace
}  // namespace mongo

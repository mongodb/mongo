// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/sasl_plain_server_conversation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"
#include "mongo/util/duration.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {
namespace {
template <typename HashBlock>
StatusWith<bool> trySCRAM(const User::CredentialData& credentials, std::string_view pwd) {
    const auto scram = credentials.scram<HashBlock>();
    if (!scram.isValid()) {
        // No stored credentials available.
        return false;
    }

    const auto decodedSalt = base64::decode(scram.salt);
    scram::Secrets<HashBlock, scram::UnlockedSecretsPolicy> secrets(scram::Presecrets<HashBlock>(
        std::string{pwd},
        std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(decodedSalt.c_str()),
                                  reinterpret_cast<const std::uint8_t*>(decodedSalt.c_str()) +
                                      decodedSalt.size()),
        scram.iterationCount));
    if (scram.storedKey !=
        base64::encode(std::string_view(reinterpret_cast<const char*>(secrets.storedKey().data()),
                                        secrets.storedKey().size()))) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Incorrect user name or password");
    }

    return true;
}
}  // namespace

StatusWith<std::tuple<bool, std::string>> SASLPlainServerMechanism::stepImpl(
    OperationContext* opCtx, std::string_view inputData) {
    if (_authenticationDatabase == "$external") {
        return Status(ErrorCodes::AuthenticationFailed,
                      "PLAIN mechanism must be used with internal users");
    }

    AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getService());

    // Expecting user input on the form: [authz-id]\0authn-id\0pwd
    std::string input = std::string{inputData};

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
    auto swUser = [&]() -> StatusWith<UserHandle> {
        ScopedCallbackTimer timer([&](Microseconds elapsed) {
            if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                LOGV2(6788606,
                      "Auth metrics report",
                      "metric"_attr = "plain_acquireUser",
                      "micros"_attr = elapsed.count());
            }
        });

        auto swRequest = makeUserRequest(opCtx);
        if (!swRequest.isOK()) {
            return swRequest.getStatus();
        }

        return authManager->acquireUser(opCtx, std::move(swRequest.getValue()));
    }();

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

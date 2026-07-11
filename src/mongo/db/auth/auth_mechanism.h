// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] auth {
using namespace std::literals::string_view_literals;

/** Wire-protocol names for supported authentication mechanisms. */
constexpr auto kMechanismMongoX509 = "MONGODB-X509"sv;
constexpr auto kMechanismSaslPlain = "PLAIN"sv;
constexpr auto kMechanismGSSAPI = "GSSAPI"sv;
constexpr auto kMechanismScramSha1 = "SCRAM-SHA-1"sv;
constexpr auto kMechanismScramSha256 = "SCRAM-SHA-256"sv;
constexpr auto kMechanismMongoAWS = "MONGODB-AWS"sv;
constexpr auto kMechanismMongoOIDC = "MONGODB-OIDC"sv;
constexpr auto kMechanismMongoDbCr = "MONGODB-CR"sv;
constexpr auto kInternalAuthFallbackMechanism = kMechanismScramSha1;

/** Typed enum of supported authentication mechanisms. */
enum class AuthMechanism {
    kMongoX509,
    kSaslPlain,
    kGSSAPI,
    kScramSha1,
    kScramSha256,
    kMongoAWS,
    kMongoOIDC,
    kMongoDbCr,  // deprecated; recognized at parse time, rejected at auth time
};

/** Return the wire-protocol string for a mechanism (e.g. "SCRAM-SHA-256"). */
std::string_view toString(AuthMechanism mechanism);

/** Parse a wire-protocol string into an AuthMechanism, or return InvalidOptions. */
StatusWith<AuthMechanism> authMechanismFromString(std::string_view s);

/** Validate that a string names a supported mechanism; returns BadValue on failure. */
Status validateAuthMechanism(std::string_view value);

}  // namespace auth
}  // namespace mongo

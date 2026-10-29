// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/auth_mechanism.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {
namespace auth {

std::string_view toString(AuthMechanism mechanism) {
    switch (mechanism) {
        case AuthMechanism::kMongoX509:
            return kMechanismMongoX509;
        case AuthMechanism::kSaslPlain:
            return kMechanismSaslPlain;
        case AuthMechanism::kGSSAPI:
            return kMechanismGSSAPI;
        case AuthMechanism::kScramSha1:
            return kMechanismScramSha1;
        case AuthMechanism::kScramSha256:
            return kMechanismScramSha256;
        case AuthMechanism::kMongoAWS:
            return kMechanismMongoAWS;
        case AuthMechanism::kMongoOIDC:
            return kMechanismMongoOIDC;
        case AuthMechanism::kMongoDbCr:
            return kMechanismMongoDbCr;
    }
    MONGO_UNREACHABLE;
}

StatusWith<AuthMechanism> authMechanismFromString(std::string_view s) {
    if (s == kMechanismMongoX509)
        return AuthMechanism::kMongoX509;
    if (s == kMechanismSaslPlain)
        return AuthMechanism::kSaslPlain;
    if (s == kMechanismGSSAPI)
        return AuthMechanism::kGSSAPI;
    if (s == kMechanismScramSha1)
        return AuthMechanism::kScramSha1;
    if (s == kMechanismScramSha256)
        return AuthMechanism::kScramSha256;
    if (s == kMechanismMongoAWS)
        return AuthMechanism::kMongoAWS;
    if (s == kMechanismMongoOIDC)
        return AuthMechanism::kMongoOIDC;
    if (s == kMechanismMongoDbCr)
        return AuthMechanism::kMongoDbCr;
    return Status{ErrorCodes::InvalidOptions,
                  str::stream() << "Unsupported authentication mechanism: " << s};
}

Status validateAuthMechanism(std::string_view value) {
    auto sw = authMechanismFromString(value);
    if (!sw.isOK())
        return {ErrorCodes::BadValue,
                str::stream() << "Unknown authentication mechanism '" << value
                              << "'. Supported mechanisms: MONGODB-X509, PLAIN, GSSAPI, "
                                 "SCRAM-SHA-1, SCRAM-SHA-256, MONGODB-AWS, MONGODB-OIDC"};
    if (sw.getValue() == AuthMechanism::kMongoDbCr)
        return Status{ErrorCodes::BadValue,
                      "MONGODB-CR is deprecated and no longer supported. Use SCRAM for "
                      "password-based authentication instead."};
    return Status::OK();
}

}  // namespace auth
}  // namespace mongo

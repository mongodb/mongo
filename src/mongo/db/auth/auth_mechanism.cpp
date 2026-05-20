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

#include "mongo/db/auth/auth_mechanism.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

namespace mongo {
namespace auth {

StringData toString(AuthMechanism mechanism) {
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

StatusWith<AuthMechanism> authMechanismFromString(StringData s) {
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

Status validateAuthMechanism(StringData value) {
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

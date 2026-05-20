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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace MONGO_MOD_PUBLIC auth {

/** Wire-protocol names for supported authentication mechanisms. */
constexpr auto kMechanismMongoX509 = "MONGODB-X509"_sd;
constexpr auto kMechanismSaslPlain = "PLAIN"_sd;
constexpr auto kMechanismGSSAPI = "GSSAPI"_sd;
constexpr auto kMechanismScramSha1 = "SCRAM-SHA-1"_sd;
constexpr auto kMechanismScramSha256 = "SCRAM-SHA-256"_sd;
constexpr auto kMechanismMongoAWS = "MONGODB-AWS"_sd;
constexpr auto kMechanismMongoOIDC = "MONGODB-OIDC"_sd;
constexpr auto kMechanismMongoDbCr = "MONGODB-CR"_sd;
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
StringData toString(AuthMechanism mechanism);

/** Parse a wire-protocol string into an AuthMechanism, or return InvalidOptions. */
StatusWith<AuthMechanism> authMechanismFromString(StringData s);

/** Validate that a string names a supported mechanism; returns BadValue on failure. */
Status validateAuthMechanism(StringData value);

}  // namespace MONGO_MOD_PUBLIC auth
}  // namespace mongo

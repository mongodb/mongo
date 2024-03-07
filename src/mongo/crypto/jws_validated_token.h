/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jwt_types_gen.h"

namespace mongo::crypto {

struct IssuerAudiencePair {
    std::string issuer;
    std::vector<std::string> audience;
};

class JWSValidatedToken {
public:
    JWSValidatedToken() = delete;

    /**
     * Constructs an instance by parsing a JWS Compact Serialization,
     * extracting key name and signature type from the header,
     * and selecting an appropriate key from the key manager.
     * Upon completion, header and body payloads
     * and parsed structs are available.
     */
    JWSValidatedToken(JWKManager* keyMgr, StringData token);

    /**
     * Extract just the Issuer name ('iss') from the token.
     */
    static StatusWith<std::string> extractIssuerFromCompactSerialization(StringData token);
    // TODO: SERVER-85968 remove extractIssuerFromCompactSerialization when
    // featureFlagOIDCMultipurposeIDP defaults to enabled

    /**
     * Extract the Issuer name ('iss') and the audience list ('aud') from the token.
     */
    static StatusWith<IssuerAudiencePair> extractIssuerAndAudienceFromCompactSerialization(
        StringData token);

    /**
     * Validates token is not expired or issued on a later date,
     * verifies it has a validator matching its keyId and finally
     * it calls validate from the validator, returning the status.
     */
    Status validate(JWKManager* keyMgr) const;

    // General read-only accessors.
    const BSONObj& getHeaderBSON() const {
        return _headerBSON;
    };
    const JWSHeader& getHeader() const {
        return _header;
    };
    const BSONObj& getBodyBSON() const {
        return _bodyBSON;
    };
    const JWT& getBody() const {
        return _body;
    };

private:
    BSONObj _headerBSON;
    JWSHeader _header;
    BSONObj _bodyBSON;
    JWT _body;
    std::string _originalToken;
};

}  // namespace mongo::crypto

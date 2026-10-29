// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] crypto {

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
    JWSValidatedToken(JWKManager* keyMgr, std::string_view token);

    /**
     * Extract the Issuer name ('iss') and the audience list ('aud') from the token.
     */
    static StatusWith<IssuerAudiencePair> extractIssuerAndAudienceFromCompactSerialization(
        std::string_view token);

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

    const std::string& getOriginalToken() const {
        return _originalToken;
    }

private:
    BSONObj _headerBSON;
    JWSHeader _header;
    BSONObj _bodyBSON;
    JWT _body;
    std::string _originalToken;
};

}  // namespace crypto
}  // namespace mongo

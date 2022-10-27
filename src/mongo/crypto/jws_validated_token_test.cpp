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

#include "mongo/crypto/jws_validated_token.h"

#include <iostream>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

namespace mongo::crypto {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER >= 0x2070000fL)

// Serialization Header: { typ: 'JWT', alg: 'RS256', kid: 'custom-key-1' }
constexpr auto modifiedTokenHeader =
    "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsImtpZCI6ImN1c3RvbS1rZXktMSJ9";

// Serialization Header: { typ: 'JWT', alg: 'RS256', kid: 'custom-key-2' }
// Serialization Body: { iss: "JWSCompactParserTest", sub: "jwsParserTest1",
//                       iat: 1661374077, exp: 2147483647,
//                       aud: ["jwt@kernel.mongodb.com"],
//                       nonce: "gdfhjj324ehj23k4", auth_time: 1661374077 }
// Expires 01/18/2038
constexpr auto validTokenHeader =
    "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsImtpZCI6ImN1c3RvbS1rZXktMiJ9";
constexpr auto validTokenBody =
    "eyJpc3MiOiJKV1NDb21wYWN0UGFyc2VyVGVzdCIsInN1YiI6Imp3c1BhcnNlclRlc3QxIiwiaWF0IjoxNjYxMzc0MDc3LC"
    "JleHAiOjIxNDc0ODM2NDcsImF1ZCI6WyJqd3RAa2VybmVsLm1vbmdvZGIuY29tIl0sIm5vbmNlIjoiZ2RmaGpqMzI0ZWhq"
    "MjNrNCIsImF1dGhfdGltZSI6MTY2MTM3NDA3N30";
constexpr auto validTokenSignature =
    "E6wxDxFrxpzt-zxjhTbtslT_T5UlMMZDfqxnoIyeDBb1d7VD9ced_"
    "yH192qfldjuR8Q4Wv5YLkjMTQ8KNXIjN313EAomd2jBxHo9zHgXd9jenVIWxF7WLI4hqWZYaO630bhoRFeQYIF4J-"
    "7fgJ9xQEJTWWi8peqXpYaCUcw2rEP-vA0oPfJhTIY67DLaTwPUExQ37kNn58Ei0ey4VWokGeY16aeyLVI-aLbh_xzwt_"
    "DEPq4Ifjj1mab4hg1m7QYfKFpezoldmC-"
    "0WJqqae9IhucUYK4T1nrAR4PBQreunIutajv0j8kMu3Mb7fBdFrfxhAzm7oeCrwPEIHRk-rDsiw";

// Serialization Header: { typ: 'JWT', alg: 'RS256', kid: 'custom-key-2' }
// Serialization Body: { iss: "JWSCompactParserTest", sub: "jwsParserTest1",
//                       iat: 1661374077, exp: 1661374677,
//                       aud: ["jwt@kernel.mongodb.com"],
//                       nonce: "gdfhjj324ehj23k4", auth_time: 1661374077 }
constexpr auto expiredTokenHeader =
    "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsImtpZCI6ImN1c3RvbS1rZXktMiJ9";
constexpr auto expiredTokenBody =
    "eyJpc3MiOiJKV1NDb21wYWN0UGFyc2VyVGVzdCIsInN1YiI6Imp3c1BhcnNlclRlc3QxIiwiaWF0IjoxNjYxMzc0MDc3LC"
    "JleHAiOjE2NjEzNzQ2NzcsImF1ZCI6WyJqd3RAa2VybmVsLm1vbmdvZGIuY29tIl0sIm5vbmNlIjoiZ2RmaGpqMzI0ZWhq"
    "MjNrNCIsImF1dGhfdGltZSI6MTY2MTM3NDA3N30";
constexpr auto expiredTokenSignature =
    "xNcutFSVjDdHQ2U1xZMb6eghjNXrozJrtWl58dk-bJhatqMotbF1OecurgqQPru2KOhY_IT4rba0F1m403Pp10WiC5-"
    "zpyhElKiBktB7U3XamVZYKDFPpr6iFGxlfBEwzbA39Y6akjEqFExhQ0wr3kR4oqVGiCoG8prPWV39-"
    "MUpgtWg8XaJK65wK3jmEHWfr2QE5mLNpLQBzifBKqhXCqR69VWyFm9FSKyYXLMgk-yH3mIFNBdZxutvWg_PZFECwdcjl-"
    "rtZX-VvzXUtNZl7Dcnn7PbtOEmSISpCdd797we4iwfAHduf5tUykiYn7_NwHD_fxCyfI8HgtRJ9VmVEQ";

BSONObj getTestJWKSet() {
    BSONObjBuilder set;
    BSONArrayBuilder keys(set.subarrayStart("keys"_sd));

    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "RSA");
        key.append("kid", "custom-key-1");
        key.append("e", "AQAB");
        key.append(
            "n",
            "ALtUlNS31SzxwqMzMR9jKOJYDhHj8zZtLUYHi3s1en3wLdILp1Uy8O6Jy0Z66tPyM1u8lke0JK5gS-40yhJ-"
            "bvqioW8CnwbLSLPmzGNmZKdfIJ08Si8aEtrRXMxpDyz4Is7JLnpjIIUZ4lmqC3MnoZHd6qhhJb1v1Qy-"
            "QGlk4NJy1ZI0aPc_uNEUM7lWhPAJABZsWc6MN8flSWCnY8pJCdIk_cAktA0U17tuvVduuFX_"
            "94763nWYikZIMJS_cTQMMVxYNMf1xcNNOVFlUSJHYHClk46QT9nT8FWeFlgvvWhlXfhsp9aNAi3pX-"
            "KxIxqF2wABIAKnhlMa3CJW41323Js");
        key.doneFast();
    }
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "RSA");
        key.append("kid", "custom-key-2");
        key.append("e", "AQAB");
        key.append(
            "n",
            "4Amo26gLJITvt62AXI7z224KfvfQjwpyREjtpA2DU2mN7pnlz-"
            "ZDu0sygwkhGcAkRPVbzpEiliXtVo2dYN4vMKLSd5BVBXhtB41bZ6OUxni48uP5txm7w8BUWv8MxzPkzyW_"
            "3dd8rOfzECdLCF5G3aA4u_XRu2ODUSAMcrxXngnNtAuC-"
            "OdqgYmvZfgFwqbU0VKNR4bbkhSrw6p9Tct6CUW04Ml4HMacZUovJKXRvNqnHcx3sy4PtVe3CyKlbb4KhBtkj1U"
            "U_"
            "cwiosz8uboBbchp7wsATieGVF8x3BUtf0ry94BGYXKbCGY_Mq-TSxcM_3afZiJA1COVZWN7d4GTEw");
        key.doneFast();
    }

    keys.doneFast();
    return set.obj();
}

TEST(JWKManager, validateTokenFromKeys) {
    auto validToken = validTokenHeader + "."_sd + validTokenBody + "."_sd + validTokenSignature;
    BSONObj keys = getTestJWKSet();

    JWKManager manager(keys);
    JWSValidatedToken validatedToken(manager, validToken);

    auto headerString = base64url::decode(validTokenHeader);
    BSONObj headerBSON = fromjson(headerString);
    auto header = JWSHeader::parse(IDLParserContext("JWTHeader"), headerBSON);

    ASSERT_BSONOBJ_EQ(validatedToken.getHeaderBSON(), headerBSON);
    ASSERT_BSONOBJ_EQ(validatedToken.getHeader().toBSON(), header.toBSON());

    auto bodyString = base64url::decode(validTokenBody);
    BSONObj bodyBSON = fromjson(bodyString);
    auto body = JWT::parse(IDLParserContext("JWT"), bodyBSON);

    ASSERT_BSONOBJ_EQ(validatedToken.getBodyBSON(), bodyBSON);
    ASSERT_BSONOBJ_EQ(validatedToken.getBody().toBSON(), body.toBSON());
}

TEST(JWKManager, failsWithExpiredToken) {
    auto expiredToken =
        expiredTokenHeader + "."_sd + expiredTokenBody + "."_sd + expiredTokenSignature;
    BSONObj keys = getTestJWKSet();

    JWKManager manager(keys);
    ASSERT_THROWS(JWSValidatedToken(manager, expiredToken), DBException);
}

TEST(JWKManager, failsWithModifiedToken) {
    auto modifiedToken =
        validTokenHeader + "."_sd + validTokenBody + "."_sd + validTokenSignature + "a"_sd;
    BSONObj keys = getTestJWKSet();

    JWKManager manager(keys);
    ASSERT_THROWS(JWSValidatedToken(manager, modifiedToken), DBException);
}

TEST(JWKManager, failsWithModifiedHeaderForADifferentKey) {
    auto modifiedToken =
        modifiedTokenHeader + "."_sd + validTokenBody + "."_sd + validTokenSignature;
    BSONObj keys = getTestJWKSet();

    JWKManager manager(keys);
    ASSERT_THROWS(JWSValidatedToken(manager, modifiedToken), DBException);
}
#endif
}  // namespace mongo::crypto

#endif

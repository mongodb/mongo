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

#include "mongo/base/string_data.h"
#include "mongo/crypto/jwk_manager_test_framework.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::crypto::test {
namespace {

BSONObj getCompleteTestJWKSet() {
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
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "EC");
        key.append("kid", "ec-prime256v1");
        key.append("crv", "P-256");
        key.append("x", "YIq56eQHNCUKUhvpbXssCWvnCHaJkD-5KKoLxwRENxc");
        key.append("y", "ZoGRVDgfGRZ8OsJN8O3DH6nFfo_LWbVK_e8Bk3ZyT1k");
        key.doneFast();
    }
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "EC");
        key.append("kid", "ec-secp384r1");
        key.append("crv", "P-384");
        key.append("x", "INQz_7Dh89R9A4ONlGgYdQKKE9ttkoe0-rPzop9x8OY7fQJ1U5cczA5lJeqAREot");
        key.append("y", "8SHZNb0g6u-ZB_gg0268dP5RzJJE13_-jYC0GyZ_48B3JGPDGcROMifXwzMPadtX");
        key.doneFast();
    }
    keys.doneFast();
    return set.obj();
}

BSONObj getPartialTestJWKSet() {
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

    keys.doneFast();
    return set.obj();
}

// Try to load keys for a supported algorithm (EC) but unsupported curve (P-521)
BSONObj getUnsupportedCurveJWKSet() {
    BSONObjBuilder set;
    BSONArrayBuilder keys(set.subarrayStart("keys"_sd));
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "EC");
        key.append("kid", "ec-secp521r1");
        key.append("crv", "P-521");
        key.append("alg", "ES512");
        key.append("x",
                   "AEu5vZ_bVcV3d5z5pLNrF1q7Jabh1ZjGN8kwdwrXnd9jRIXTD2t-"
                   "T3B2TAajO2jzQhOq29FIeycUrcgFCp8ItAtX");
        key.append("y",
                   "AOaLCQqebglolZIkUHX-APj_gYZdG0DyC0CL9AZagtKcHMQMuUjhofka-"
                   "hVCr0xKIvQlhq3U3Y5ggR1IR8vpQ0R9");
        key.doneFast();
    }
    keys.doneFast();
    return set.obj();
}

// Try to load keys for an unsupported protocol HS256
BSONObj getUnsupportedProtocolJWKSet() {
    BSONObjBuilder set;
    BSONArrayBuilder keys(set.subarrayStart("keys"_sd));
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "EC");
        key.append("kid", "ec-prime256v1");
        key.append("crv", "P-256");
        key.append("x", "YIq56eQHNCUKUhvpbXssCWvnCHaJkD-5KKoLxwRENxc");
        key.append("y", "ZoGRVDgfGRZ8OsJN8O3DH6nFfo_LWbVK_e8Bk3ZyT1k");
        key.doneFast();
    }
    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "oct");
        key.append("kid", "hs256");
        key.append("alg", "HS256");
        key.append("k", "MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIK");
        key.doneFast();
    }
    keys.doneFast();
    return set.obj();
}

void assertCorrectKeys(JWKManager* manager, BSONObj data) {
    const auto& currentKeys = manager->getKeys();
    for (const auto& key : data["keys"_sd].Obj()) {
        auto currentKey = currentKeys.find(key["kid"_sd].str());
        ASSERT(currentKey != currentKeys.end());
        ASSERT_BSONOBJ_EQ(key.Obj(), currentKey->second);
    }

    for (const auto& key : data["keys"_sd].Obj()) {
        auto validator = uassertStatusOK(manager->getValidator(key["kid"_sd].str()));
        ASSERT(validator);
    }
}

TEST_F(JWKManagerTest, parseJWKSetBasicFromSource) {
    RAIIServerParameterControllerForTest quiesceController("JWKSMinimumQuiescePeriodSecs", 0);

    auto data = getCompleteTestJWKSet();
    jwksFetcher()->setKeys(getCompleteTestJWKSet());

    // Initially, set the fetcher to fail. This should cause the JWKManager to contain no keys
    // even after loadKeys() is called.
    jwksFetcher()->setShouldFail(true);
    ASSERT_EQ(jwkManager()->size(), 0);
    ASSERT_NOT_OK(jwkManager()->loadKeys());
    ASSERT_EQ(jwkManager()->size(), 0);

    // Then, set the fetcher to succeed. The subsequent call to loadKeys() should result in the
    // keys getting updated correctly.
    jwksFetcher()->setShouldFail(false);
    ASSERT_OK(jwkManager()->loadKeys());
    ASSERT_EQ(jwkManager()->size(), 4);

    BSONObjBuilder successfulLoadKeysBob;
    jwkManager()->serialize(&successfulLoadKeysBob);
    ASSERT_BSONOBJ_EQ(successfulLoadKeysBob.obj(), data);
    assertCorrectKeys(jwkManager(), data);

    // Finally, set the fetcher to fail again. The subsequent call to loadKeys() should fail but
    // leave the manager's keys untouched.
    jwksFetcher()->setShouldFail(true);
    ASSERT_NOT_OK(jwkManager()->loadKeys());
    ASSERT_EQ(jwkManager()->size(), 4);

    BSONObjBuilder failedLoadKeysBob;
    jwkManager()->serialize(&failedLoadKeysBob);
    ASSERT_BSONOBJ_EQ(failedLoadKeysBob.obj(), data);
    assertCorrectKeys(jwkManager(), data);
}

TEST_F(JWKManagerTest, JWKSFetcherQuiesce) {
    RAIIServerParameterControllerForTest quiesceController("JWKSMinimumQuiescePeriodSecs", 5);

    // Initially the fetcher will contain no keys.
    ASSERT_EQ(jwkManager()->size(), 0);

    // Update keys at time < quiesce period. Fetcher will JIT update since it is the initial key
    // load.
    jwksFetcher()->setKeys(getPartialTestJWKSet());
    getClock()->advance(Seconds{3});
    ASSERT_NOT_OK(jwkManager()->getValidator("custom-key-2"_sd));
    ASSERT_NOT_OK(jwkManager()->getValidator("ec-prime256v1"_sd));
    ASSERT_NOT_OK(jwkManager()->getValidator("ec-secp384r1"_sd));
    ASSERT_EQ(jwkManager()->size(), 1);

    // Add second key at time < quiesce period. Fetcher should not update.
    jwksFetcher()->setKeys(getCompleteTestJWKSet());
    getClock()->advance(Seconds{3});
    ASSERT_OK(jwkManager()->getValidator("custom-key-1"_sd));
    ASSERT_NOT_OK(jwkManager()->getValidator("custom-key-2"_sd));
    ASSERT_EQ(jwkManager()->size(), 1);

    // Advance clock further, keys will now be JIT loaded.
    getClock()->advance(Seconds{3});
    ASSERT_OK(jwkManager()->getValidator("custom-key-1"_sd));
    ASSERT_OK(jwkManager()->getValidator("custom-key-2"_sd));
    ASSERT_OK(jwkManager()->getValidator("ec-prime256v1"_sd));
    ASSERT_OK(jwkManager()->getValidator("ec-secp384r1"_sd));
    ASSERT_EQ(jwkManager()->size(), 4);
}

TEST_F(JWKManagerTest, parseJWKSetUnsupportedCurve) {
    auto data = getUnsupportedCurveJWKSet();
    jwksFetcher()->setKeys(data);
    Status st = jwkManager()->loadKeys();
    ASSERT_NOT_OK(st);
    ASSERT_EQ(st.code(), 10858402);
    ASSERT_NOT_OK(jwkManager()->getValidator("ec-secp521r1"_sd));
}

TEST_F(JWKManagerTest, parseJWKSetUnsupportedProtocol) {
    auto data = getUnsupportedProtocolJWKSet();
    jwksFetcher()->setKeys(data);
    Status st = jwkManager()->loadKeys();
    // Unsupported protocol should be skipped
    ASSERT_OK(st);
    ASSERT_OK(jwkManager()->getValidator("ec-prime256v1"_sd));
    ASSERT_NOT_OK(jwkManager()->getValidator("hs256"_sd));
}

}  // namespace
}  // namespace mongo::crypto::test

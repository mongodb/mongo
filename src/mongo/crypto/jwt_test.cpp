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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jwks_fetcher_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client.h"

namespace mongo::crypto {
namespace {

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

TEST(JWKManager, parseJWKSetBasicFromSource) {
    auto httpClient = HttpClient::createWithoutConnectionPool();
    httpClient->setHeaders({"Accept: */*"});

    auto data = getTestJWKSet();
    JWKManager manager(std::make_unique<MockJWKSFetcher>(data), true /* loadAtStartup */);

    BSONObjBuilder bob;
    manager.serialize(&bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), data);

    const auto& currentKeys = manager.getKeys();
    for (const auto& key : data["keys"_sd].Obj()) {
        auto currentKey = currentKeys.find(key["kid"_sd].str());
        ASSERT(currentKey != currentKeys.end());
        ASSERT_BSONOBJ_EQ(key.Obj(), currentKey->second);
    }

    for (const auto& key : data["keys"_sd].Obj()) {
        auto validator = uassertStatusOK(manager.getValidator(key["kid"_sd].str()));
        ASSERT(validator);
    }
}

}  // namespace
}  // namespace mongo::crypto

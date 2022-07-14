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

#include "mongo/crypto/jwk_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::crypto {
namespace {

// Test key source: RFC 7515 "JSON Web Key" Appendix B.
constexpr auto k512BitKeyE = "AQAB"_sd;
constexpr auto k512BitKeyN =
    "vrjOfz9Ccdgx5nQudyhdoR17V-IubWMeOZCwX_jj0hgAsz2J_pqYW08PLbK_PdiVGKPrqzmDIsL"
    "I7sA25VEnHU1uCLNwBuUiCO11_-7dYbsr4iJmG0Qu2j8DsVyT1azpJC_NG84Ty5KKthuCaPod7i"
    "I7w0LK9orSMhBEwwZDCxTWq4aYWAchc8t-emd9qOvWtVMDC2BXksRngh6X5bUYLy6AyHKvj-nUy"
    "1wgzjYQDwHMTplCoLtU-o-8SNnZ1tmRoGE9uJkBLdh5gFENabWnU5m1ZqZPdwS-qo-meMvVfJb6"
    "jJVWRpl2SUtCnYG2C32qvbWbjZ_jBPD5eunqsIo1vQ"_sd;

BSONObj getTestJWKSet(StringData e, StringData n) {
    BSONObjBuilder set;
    BSONArrayBuilder keys(set.subarrayStart("keys"_sd));

    {
        BSONObjBuilder key(keys.subobjStart());
        key.append("kty", "RSA");
        key.append("kid", "numberOneKey");
        key.append("e", e);
        key.append("n", n);
        key.doneFast();
    }

    keys.doneFast();
    return set.obj();
}

TEST(JWKManager, parseJWKSetBasic) {
    // Parse the test JWKSet and pull out numberOneKey, then compare it to the inputs.
    auto basic = getTestJWKSet(k512BitKeyE, k512BitKeyN);
    JWKManager manager(basic);
    auto key = manager.getKey("numberOneKey");

    ASSERT_BSONOBJ_EQ(key.toBSON(), basic["keys"_sd].Obj()[0].Obj());
}

TEST(JWKManager, parseJWKSetEmptyComponents) {
    auto emptyE = getTestJWKSet("", k512BitKeyN);
    ASSERT_THROWS_WHAT(JWKManager(emptyE), DBException, "Public key component invalid: ");

    auto emptyN = getTestJWKSet(k512BitKeyE, "");
    ASSERT_THROWS_WHAT(JWKManager(emptyN),
                       DBException,
                       "Key scale is smaller (0 bits) than minimum required: 512");
}

}  // namespace
}  // namespace mongo::crypto

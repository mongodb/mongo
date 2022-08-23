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

#include "mongo/crypto/rsa_public_key.h"

#include <iostream>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"

namespace mongo::crypto {

TEST(RSAPublickKeyTest, rsaKeyDecode) {
    const auto keyID = "0UhWwyvtfIdxPvR9zCWYJB5_AM0LE2qc6RGOcI0cQjw"_sd;
    const auto e = "AQAB"_sd;
    const auto n =
        "ionlnDDd4AG2rRFgjEowRUiZ8x7LTfM-cwwBTuV4TAWgZKb3RycprPwdPODtbKSxnyoM6-Bi-"
        "qM0FInx13vsC3h3xxzMIreH-vRQPWWocsJ6CZrgfbXyUclcLzgJX_E2V_6hpG0CeUBfNYsgfgwm4Y_"
        "wjUu3HKsKPdIPqjf6zdrgv8W3OySt-QSFVBy_OQXraZ2wA7gJPyPmNhBr8L9M3AYRS_"
        "E1XRpsldMSrIe8bfxGyP2B9txiUQXIycWLC-e172SPjAjdUyaK3YLqGRtki6EgQ3qlzRPjoQheE-"
        "r3l62UaaAgHOo6FercdjdsIzT2-vhqZMQk59WhGuvygymiLw"_sd;
    RsaPublicKey key(keyID, e, n);

    std::string strE;
    strE.assign(key.getE().data(), key.getE().length());
    ASSERT_EQ(base64url::encode(strE), e);

    std::string strN;
    strN.assign(key.getN().data(), key.getN().length());
    ASSERT_EQ(base64url::encode(strN), n);
}
}  // namespace mongo::crypto

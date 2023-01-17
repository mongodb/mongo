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
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client.h"

namespace mongo::crypto {
namespace {

constexpr auto source = "https://mongodbcorp.okta.com/oauth2/ausfgfhg2j9rtr0nT297/v1/keys"_sd;

TEST(JWKManager, parseJWKSetBasicFromSource) {
    auto httpClient = HttpClient::createWithoutConnectionPool();
    httpClient->setHeaders({"Accept: */*"});

    DataBuilder getJWKs = httpClient->get(source);

    ConstDataRange cdr = getJWKs.getCursor();
    StringData str;
    cdr.readInto<StringData>(&str);

    BSONObj data = fromjson(str);
    JWKManager manager(source);

    BSONObjBuilder bob;
    manager.serialize(&bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), data);

    const auto& initialKeys = manager.getInitialKeys();
    for (const auto& key : data["keys"_sd].Obj()) {
        auto initialKey = initialKeys.find(key["kid"_sd].str());
        ASSERT(initialKey != initialKeys.end());
        ASSERT_BSONOBJ_EQ(key.Obj(), initialKey->second);
    }

    for (const auto& key : data["keys"_sd].Obj()) {
        auto validator = uassertStatusOK(manager.getValidator(key["kid"_sd].str()));
        ASSERT(validator);
    }
}

}  // namespace
}  // namespace mongo::crypto

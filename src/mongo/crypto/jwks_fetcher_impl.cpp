/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/crypto/jwks_fetcher_impl.h"

#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/oauth_authorization_server_metadata_gen.h"
#include "mongo/db/auth/oauth_discovery_factory.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/str.h"

namespace mongo::crypto {

JWKSet JWKSFetcherImpl::fetch() {
    try {
        auto makeHTTPClient = []() {
            auto httpClient = HttpClient::createWithoutConnectionPool();
            httpClient->setHeaders({"Accept: */*"});
            httpClient->allowInsecureHTTP(getTestCommandsEnabled());
            return httpClient;
        };

        OAuthDiscoveryFactory discoveryFactory(makeHTTPClient());
        auto metadata = discoveryFactory.acquire(_issuer);

        auto jwksUri = metadata.getJwksUri();
        auto getJWKs = makeHTTPClient()->get(jwksUri);

        ConstDataRange cdr = getJWKs.getCursor();
        StringData str;
        cdr.readInto<StringData>(&str);

        return JWKSet::parseOwned(IDLParserContext("JWKSet"), fromjson(str));
    } catch (DBException& ex) {
        ex.addContext(str::stream() << "Failed loading keys from " << _issuer);
        throw;
    }
}

}  // namespace mongo::crypto

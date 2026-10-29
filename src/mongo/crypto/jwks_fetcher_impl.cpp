// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/jwks_fetcher_impl.h"

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
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::crypto {

JWKSFetcherImpl::JWKSFetcherImpl(ClockSource* clock, std::string_view issuer)
    : _issuer(issuer), _clock(clock), _lastAttemptedFetchTime(Date_t::min()) {}

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

        _lastAttemptedFetchTime = _clock->now();
        auto jwksUri = metadata.getJwksUri();
        auto getJWKs = makeHTTPClient()->get(jwksUri);

        ConstDataRange cdr = getJWKs.getCursor();
        std::string_view str;
        cdr.readInto<std::string_view>(&str);

        return JWKSet::parseOwned(fromjson(str), IDLParserContext("JWKSet"));
    } catch (DBException& ex) {
        ex.addContext(str::stream() << "Failed loading keys from " << _issuer);
        throw;
    }
}

}  // namespace mongo::crypto

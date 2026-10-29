// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/oauth_discovery_factory.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/bson/json.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {

OAuthAuthorizationServerMetadata OAuthDiscoveryFactory::acquire(std::string_view issuer) {
    // RFC8414 declares that the well-known addresses defined by OpenID Connect are valid for
    // compliant clients for legacy purposes. Newer clients should use
    // '.well-known/oauth-authorization-server'. However, that endpoint uses a different URL
    // construction scheme which doesn't seem to work with any of the authorization servers we've
    // tested.

    // Some issuers URL will end with '/', we should remove it since we add it when forming the
    // configuration endpoint.
    if (issuer.ends_with('/')) {
        issuer.remove_suffix(1);
    }

    auto openIDConfiguationEndpoint = fmt::format("{}/.well-known/openid-configuration", issuer);

    DataBuilder results = _client->get(openIDConfiguationEndpoint);
    std::string_view textResult =
        uassertStatusOK(results.getCursor().readAndAdvanceNoThrow<std::string_view>());
    auto metadata = OAuthAuthorizationServerMetadata::parseOwned(fromjson(textResult),
                                                                 IDLParserContext("metadata"));

    // RFC 8414 §3.3: "The 'issuer' value returned MUST be identical to the issuer identifier value
    // into which the well-known URI string was inserted to create the URL used to retrieve the
    // metadata. If these values are not identical, the data contained in the response MUST NOT be
    // used." Enforcing this prevents a malicious server from pointing the client at an
    // attacker-controlled discovery document whose endpoints redirect client-side requests
    // elsewhere.
    std::string_view returnedIssuer = metadata.getIssuer();
    if (returnedIssuer.ends_with('/')) {
        returnedIssuer.remove_suffix(1);
    }
    uassert(ErrorCodes::BadValue,
            fmt::format("Discovered metadata issuer '{}' does not match the expected issuer '{}'",
                        returnedIssuer,
                        issuer),
            returnedIssuer == issuer);

    return metadata;
}

}  // namespace mongo

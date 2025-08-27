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

#include "mongo/db/auth/oauth_discovery_factory.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {

OAuthAuthorizationServerMetadata OAuthDiscoveryFactory::acquire(StringData issuer) {
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
    StringData textResult =
        uassertStatusOK(results.getCursor().readAndAdvanceNoThrow<StringData>());
    return OAuthAuthorizationServerMetadata::parseOwned(fromjson(textResult),
                                                        IDLParserContext("metadata"));
}

}  // namespace mongo

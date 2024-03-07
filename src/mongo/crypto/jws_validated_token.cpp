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

#include <boost/optional.hpp>
#include <cstddef>
#include <memory>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo::crypto {
namespace {
struct ParsedTokenView {
    StringData header;
    StringData body;
    StringData signature;

    StringData payload;
};

// Split "header.body.signature" into {"header", "body", "signature", "header.body"}
ParsedTokenView parseSignedToken(StringData token) {
    ParsedTokenView pt;

    auto split = token.find('.', 0);
    uassert(8039401, "Missing JWS delimiter", split != std::string::npos);
    pt.header = token.substr(0, split);
    auto pos = split + 1;

    split = token.find('.', pos);
    uassert(8039402, "Missing JWS delimiter", split != std::string::npos);
    pt.body = token.substr(pos, split - pos);
    pt.payload = token.substr(0, split);
    pos = split + 1;

    split = token.find('.', pos);
    uassert(8039403, "Too many delimiters in JWS token", split == std::string::npos);
    pt.signature = token.substr(pos);
    return pt;
}
}  // namespace
Status JWSValidatedToken::validate(JWKManager* keyMgr) const {
    const auto now = Date_t::now();

    // Clock times across the network may differ and `nbf` is likely to be
    // at or near the issue time, so provide a reasonable skew allowance.
    constexpr Seconds kNotBeforeSkewMax{60};
    if (_body.getNotBefore().get_value_or(Date_t::min()) > (now + kNotBeforeSkewMax)) {
        return Status{ErrorCodes::BadValue, "Token not yet valid"};
    }

    // Expiration we choose not to skew, as the client can be prompted to
    // create a new token for reauth, and indeed would since the token expiration
    // mechanism would immediately reject this token before the command dispatch
    // would have an opportunity to kick in.
    if (_body.getExpiration() < now) {
        return Status{ErrorCodes::BadValue, "Token is expired"};
    }

    auto tokenSplit = parseSignedToken(_originalToken);
    auto signature = base64url::decode(tokenSplit.signature);
    auto payload = tokenSplit.payload;

    auto swValidator = keyMgr->getValidator(_header.getKeyId());
    if (!swValidator.isOK()) {
        auto status = swValidator.getStatus();
        if (status.code() == ErrorCodes::NoSuchKey) {
            return Status{ErrorCodes::BadValue,
                          str::stream() << "Unknown JWT keyId << '" << _header.getKeyId() << "'"};
        }
        return status;
    }

    return swValidator.getValue().get()->validate(_header.getAlgorithm(), payload, signature);
}

JWSValidatedToken::JWSValidatedToken(JWKManager* keyMgr, StringData token)
    : _originalToken(token.toString()) {
    auto tokenSplit = parseSignedToken(token);

    auto headerString = base64url::decode(tokenSplit.header);
    _headerBSON = fromjson(headerString);
    _header = JWSHeader::parse(IDLParserContext("JWSHeader"), _headerBSON);
    uassert(7095401, "Unknown type of token", !_header.getType() || _header.getType() == "JWT"_sd);

    auto bodyString = base64url::decode(tokenSplit.body);
    _bodyBSON = fromjson(bodyString);
    _body = JWT::parse(IDLParserContext("JWT"), _bodyBSON);

    uassertStatusOK(validate(keyMgr));
};

StatusWith<std::string> JWSValidatedToken::extractIssuerFromCompactSerialization(
    StringData token) try {
    auto tokenSplit = parseSignedToken(token);
    auto payload = fromjson(base64url::decode(tokenSplit.body));
    return JWT::parse(IDLParserContext{"JWT"}, payload).getIssuer().toString();
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<IssuerAudiencePair> JWSValidatedToken::extractIssuerAndAudienceFromCompactSerialization(
    StringData token) try {
    auto tokenSplit = parseSignedToken(token);
    auto payload = fromjson(base64url::decode(tokenSplit.body));
    auto jwt = JWT::parse(IDLParserContext{"JWT"}, payload);

    IssuerAudiencePair pair;
    pair.issuer = jwt.getIssuer().toString();

    auto& audience = jwt.getAudience();
    if (std::holds_alternative<std::string>(audience)) {
        pair.audience.push_back(std::get<std::string>(audience));
    } else {
        pair.audience = std::get<std::vector<std::string>>(audience);
    }
    return pair;
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo::crypto

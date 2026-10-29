// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/jws_validated_token.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::crypto {
using namespace std::literals::string_view_literals;
namespace {
struct ParsedTokenView {
    std::string_view header;
    std::string_view body;
    std::string_view signature;

    std::string_view payload;
};

// Split "header.body.signature" into {"header", "body", "signature", "header.body"}
ParsedTokenView parseSignedToken(std::string_view token) {
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

JWSValidatedToken::JWSValidatedToken(JWKManager* keyMgr, std::string_view token)
    : _originalToken(std::string{token}) {
    auto tokenSplit = parseSignedToken(token);

    auto headerString = base64url::decode(tokenSplit.header);
    _headerBSON = fromjson(headerString);
    _header = JWSHeader::parse(_headerBSON, IDLParserContext("JWSHeader"));
    uassert(7095401, "Unknown type of token", !_header.getType() || _header.getType() == "JWT"sv);

    auto bodyString = base64url::decode(tokenSplit.body);
    _bodyBSON = fromjson(bodyString);
    _body = JWT::parse(_bodyBSON, IDLParserContext("JWT"));

    uassertStatusOK(validate(keyMgr));
};

StatusWith<IssuerAudiencePair> JWSValidatedToken::extractIssuerAndAudienceFromCompactSerialization(
    std::string_view token) try {
    auto tokenSplit = parseSignedToken(token);
    auto payload = fromjson(base64url::decode(tokenSplit.body));
    auto jwt = JWT::parse(payload, IDLParserContext{"JWT"});

    IssuerAudiencePair pair;
    pair.issuer = std::string{jwt.getIssuer()};

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

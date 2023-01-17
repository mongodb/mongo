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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"

namespace mongo::crypto {
namespace {
struct ParsedToken {
    StringData token[3];
    StringData payload;
};

// Split "header.body.signature" into {"header", "body", "signature", "header.body"}
ParsedToken parseSignedToken(StringData token) {
    ParsedToken pt;
    std::size_t split, pos = 0;
    for (int i = 0; i < 3; ++i) {
        split = token.find('.', pos);
        pt.token[i] = token.substr(pos, split - pos);
        pos = split + 1;

        if (i == 1) {
            // Payload: encoded header + '.' + encoded body
            pt.payload = token.substr(0, split);
        }
    }

    uassert(7095400, "Unknown format of token", split == std::string::npos);
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
    auto signature = base64url::decode(tokenSplit.token[2]);
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

    auto headerString = base64url::decode(tokenSplit.token[0]);
    _headerBSON = fromjson(headerString);
    _header = JWSHeader::parse(IDLParserContext("JWSHeader"), _headerBSON);
    uassert(7095401, "Unknown type of token", !_header.getType() || _header.getType() == "JWT"_sd);

    auto bodyString = base64url::decode(tokenSplit.token[1]);
    _bodyBSON = fromjson(bodyString);
    _body = JWT::parse(IDLParserContext("JWT"), _bodyBSON);

    uassertStatusOK(validate(keyMgr));
};

StatusWith<std::string> JWSValidatedToken::extractIssuerFromCompactSerialization(
    StringData token) try {
    auto tokenSplit = parseSignedToken(token);
    auto payload = fromjson(base64url::decode(tokenSplit.token[1]));
    return JWT::parse(IDLParserContext{"JWT"}, payload).getIssuer().toString();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo::crypto

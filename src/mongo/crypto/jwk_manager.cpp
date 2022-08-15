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

#include "mongo/crypto/jwk_manager.h"

#include "mongo/bson/json.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::crypto {
namespace {
constexpr auto kMinKeySizeBytes = 512 >> 3;

// Strip insignificant leading zeroes to determine the key's true size.
StringData reduceInt(StringData value) {
    std::size_t ofs = 0;
    while ((ofs < value.size()) && (value[ofs] == 0)) {
        ++ofs;
    }
    return value.substr(ofs);
}

}  // namespace

JWKManager::JWKManager(BSONObj data) {
    auto keys = JWKSet::parse(IDLParserContext{"JWKSet"}, data);
    for (const auto& key : keys.getKeys()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Only RSA key types are accepted at this time",
                key.getType() == "RSA"_sd);
        uassert(ErrorCodes::BadValue, "Key ID must be non-empty", !key.getKeyId().empty());

        // Sanity check so that we don't load a dangerously small key.
        auto N = reduceInt(key.getN());
        uassert(ErrorCodes::BadValue,
                str::stream() << "Key scale is smaller (" << (N.size() << 3)
                              << " bits) than minimum required: " << (kMinKeySizeBytes << 3),
                N.size() >= kMinKeySizeBytes);

        // Sanity check so that we don't load an insensible encrypt component.
        auto E = reduceInt(key.getE());
        uassert(ErrorCodes::BadValue,
                str::stream() << "Public key component invalid: " << base64url::encode(key.getE()),
                (E.size() > 1) || ((E.size() == 1) && (E[0] >= 3)));

        auto keyId = key.getKeyId().toString();
        uassert(ErrorCodes::DuplicateKey,
                str::stream() << "Key IDs must be unique, duplicate '" << keyId << "'",
                _keys.find(keyId) == _keys.end());

        LOGV2_DEBUG(6766000, 5, "Loaded JWK Key", "kid"_attr = key.getKeyId());
        _keys.insert({keyId, {key.getKeyId(), {E.rawData(), E.size()}, {N.rawData(), N.size()}}});
    }
}

const RsaPublicKey& JWKManager::getKey(StringData keyId) const {
    auto it = _keys.find(keyId.toString());
    uassert(
        ErrorCodes::NoSuchKey, str::stream() << "Unknown key '" << keyId << "'", it != _keys.end());
    return it->second;
}

}  // namespace mongo::crypto

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
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"
#include "mongo/util/net/http_client.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::crypto {
namespace {
constexpr auto kMinKeySizeBytes = 2048 >> 3;
using SharedValidator = std::shared_ptr<JWSValidator>;
using SharedMap = std::map<std::string, SharedValidator>;

// Strip insignificant leading zeroes to determine the key's true size.
StringData reduceInt(StringData value) {
    std::size_t ofs = 0;
    while ((ofs < value.size()) && (value[ofs] == 0)) {
        ++ofs;
    }
    return value.substr(ofs);
}

}  // namespace

JWKManager::JWKManager(StringData source) : _keyURI(source) {
    try {
        auto httpClient = HttpClient::createWithoutConnectionPool();
        httpClient->setHeaders({"Accept: */*"});
        httpClient->allowInsecureHTTP(getTestCommandsEnabled());

        auto getJWKs = httpClient->get(source);

        ConstDataRange cdr = getJWKs.getCursor();
        StringData str;
        cdr.readInto<StringData>(&str);

        BSONObj data = fromjson(str);
        _setAndValidateKeys(data);
    } catch (const DBException& ex) {
        // throws
        uassertStatusOK(
            ex.toStatus().withContext(str::stream() << "Failed loading keys from " << source));
    }
}

JWKManager::JWKManager(BSONObj keys) {
    _setAndValidateKeys(keys);
}

StatusWith<BSONObj> JWKManager::getKey(StringData keyId) const {
    auto it = _keyMaterial.find(keyId.toString());
    if (it == _keyMaterial.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "Unknown key '" << keyId << "'"};
    }
    return it->second;
}

SharedValidator JWKManager::getValidator(StringData keyId) const {
    auto it = _validators->find(keyId.toString());

    // TODO: SERVER-71195, refresh keys from the endpoint and try to get the validator again.
    // If still no key is found throw a uassert.
    uassert(ErrorCodes::NoSuchKey,
            str::stream() << "Unknown key '" << keyId << "'",
            it != _validators->end());
    return it->second;
}

void JWKManager::_setAndValidateKeys(const BSONObj& keys) {
    _validators = std::make_shared<SharedMap>();
    auto keysParsed = JWKSet::parse(IDLParserContext("JWKSet"), keys);

    for (const auto& key : keysParsed.getKeys()) {
        auto JWK = JWK::parse(IDLParserContext("JWK"), key);
        uassert(ErrorCodes::BadValue,
                str::stream() << "Only RSA key types are accepted at this time",
                JWK.getType() == "RSA"_sd);
        uassert(ErrorCodes::BadValue, "Key ID must be non-empty", !JWK.getKeyId().empty());

        auto RSAKey = JWKRSA::parse(IDLParserContext("JWKRSA"), key);

        // Sanity check so that we don't load a dangerously small key.
        auto N = reduceInt(RSAKey.getModulus());
        uassert(ErrorCodes::BadValue,
                str::stream() << "Key scale is smaller (" << (N.size() << 3)
                              << " bits) than minimum required: " << (kMinKeySizeBytes << 3),
                N.size() >= kMinKeySizeBytes);

        // Sanity check so that we don't load an insensible encrypt component.
        auto E = reduceInt(RSAKey.getPublicExponent());
        uassert(ErrorCodes::BadValue,
                str::stream() << "Public key component invalid: "
                              << base64url::encode(RSAKey.getPublicExponent()),
                (E.size() > 1) || ((E.size() == 1) && (E[0] >= 3)));

        auto keyId = RSAKey.getKeyId().toString();
        uassert(ErrorCodes::DuplicateKeyId,
                str::stream() << "Key IDs must be unique, duplicate '" << keyId << "'",
                _keyMaterial.find(keyId) == _keyMaterial.end());

        LOGV2_DEBUG(6766000, 5, "Loaded JWK Key", "kid"_attr = RSAKey.getKeyId());
        _keyMaterial.insert({keyId, key.copy()});

        auto swValidator = JWSValidator::create(JWK.getType(), key);
        uassertStatusOK(swValidator.getStatus());
        SharedValidator shValidator = std::move(swValidator.getValue());

        _validators->insert({keyId, shValidator});
        LOGV2_DEBUG(7070202, 3, "Loaded JWK key", "kid"_attr = keyId, "typ"_attr = JWK.getType());
    }
}

std::vector<std::string> JWKManager::getKeyIds() const {
    std::vector<std::string> ids;
    std::transform(_validators->cbegin(),
                   _validators->cend(),
                   std::back_inserter(ids),
                   [](const auto& it) { return it.first; });
    return ids;
}

}  // namespace mongo::crypto

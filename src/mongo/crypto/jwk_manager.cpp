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
using SharedValidatorMap = std::map<std::string, SharedValidator>;

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
    _loadKeysFromUri(true /* isInitialLoad */);
}

JWKManager::JWKManager(BSONObj keys) {
    _setAndValidateKeys(keys, true /* isInitialLoad */);
}

StatusWith<SharedValidator> JWKManager::getValidator(StringData keyId) {
    auto currentValidators = _validators;
    auto it = currentValidators->find(keyId.toString());
    if (it == currentValidators->end()) {
        // If the JWKManager has been initialized with an URI, try refreshing.
        if (_keyURI) {
            _loadKeysFromUri(false /* isInitialLoad */);
            currentValidators = _validators;
            it = currentValidators->find(keyId.toString());
        }

        // If it still cannot be found, return an error.
        if (it == currentValidators->end()) {
            return {ErrorCodes::NoSuchKey, str::stream() << "Unknown key '" << keyId << "'"};
        }
    }
    return it->second;
}

void JWKManager::_setAndValidateKeys(const BSONObj& keys, bool isInitialLoad) {
    auto newValidators = std::make_shared<SharedValidatorMap>();
    auto newKeyMaterial = std::make_shared<KeyMap>();

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
                newKeyMaterial->find(keyId) == newKeyMaterial->end());

        // Does not need to be loaded atomically because isInitialLoad is only true when invoked
        // from the constructor, so there will not be any concurrent reads.
        if (isInitialLoad) {
            _initialKeyMaterial.insert({keyId, key.copy()});
        }

        newKeyMaterial->insert({keyId, key.copy()});

        auto swValidator = JWSValidator::create(JWK.getType(), key);
        uassertStatusOK(swValidator.getStatus());
        SharedValidator shValidator = std::move(swValidator.getValue());

        newValidators->insert({keyId, shValidator});
        LOGV2_DEBUG(7070202, 3, "Loaded JWK key", "kid"_attr = keyId, "typ"_attr = JWK.getType());
    }

    // A mutex is not used here because no single thread consumes both _validators and _keyMaterial.
    // _keyMaterial is used purely for reflection of current key state while _validators is used
    // for actual signature verification with the keys. Therefore, it's safe to update each
    // atomically rather than both under a mutex.
    std::atomic_exchange(&_validators, std::move(newValidators));    // NOLINT
    std::atomic_exchange(&_keyMaterial, std::move(newKeyMaterial));  // NOLINT
}

void JWKManager::_loadKeysFromUri(bool isInitialLoad) {
    try {
        auto httpClient = HttpClient::createWithoutConnectionPool();
        httpClient->setHeaders({"Accept: */*"});
        httpClient->allowInsecureHTTP(getTestCommandsEnabled());

        invariant(_keyURI);
        auto getJWKs = httpClient->get(_keyURI.value());

        ConstDataRange cdr = getJWKs.getCursor();
        StringData str;
        cdr.readInto<StringData>(&str);

        BSONObj data = fromjson(str);
        _setAndValidateKeys(data, isInitialLoad);
    } catch (const DBException& ex) {
        // throws
        uassertStatusOK(ex.toStatus().withContext(str::stream() << "Failed loading keys from "
                                                                << _keyURI.value()));
    }
}

void JWKManager::serialize(BSONObjBuilder* bob) const {
    std::vector<BSONObj> keyVector;
    keyVector.reserve(size());

    auto currentKeys = _keyMaterial;
    std::transform(currentKeys->begin(),
                   currentKeys->end(),
                   std::back_inserter(keyVector),
                   [](const auto& keyEntry) { return keyEntry.second; });

    JWKSet jwks(keyVector);
    jwks.serialize(bob);
}

}  // namespace mongo::crypto

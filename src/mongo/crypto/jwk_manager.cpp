// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/jwk_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_parameters_gen.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo::crypto {
using namespace std::literals::string_view_literals;
namespace {
constexpr auto kMinKeySizeBytes = 2048 >> 3;
using SharedValidator = std::shared_ptr<JWSValidator>;
using SharedValidatorMap = std::map<std::string, SharedValidator>;

// Strip insignificant leading zeroes to determine the key's true size.
std::string_view reduceInt(std::string_view value) {
    std::size_t ofs = 0;
    while ((ofs < value.size()) && (value[ofs] == 0)) {
        ++ofs;
    }
    return value.substr(ofs);
}

}  // namespace

JWKManager::JWKManager(std::unique_ptr<JWKSFetcher> fetcher)
    : _fetcher(std::move(fetcher)),
      _keyMaterial(std::make_shared<KeyMap>()),
      _validators(std::make_shared<SharedValidatorMap>()),
      _isKeyModified(false) {}

StatusWith<SharedValidator> JWKManager::getValidator(std::string_view keyId) {
    auto currentValidators = std::atomic_load(&_validators);  // NOLINT
    auto it = currentValidators->find(std::string{keyId});

    if (it == currentValidators->end()) {
        // We were asked to handle an unknown keyId. Try refreshing, to see if the JWKS has been
        // updated.
        LOGV2_DEBUG(7938400,
                    3,
                    "Could not locate key in key cache, attempting key cache refresh",
                    "keyId"_attr = keyId);
        auto loadKeysStatus = loadKeys();
        if (!loadKeysStatus.isOK()) {
            LOGV2_WARNING(7938401,
                          "Failed just-in-time key cache refresh",
                          "error"_attr = loadKeysStatus.reason());
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "Unknown key '" << keyId
                                  << "': just-in-time refresh failed: " << loadKeysStatus.reason()};
        }

        currentValidators = std::atomic_load(&_validators);  // NOLINT
        it = currentValidators->find(std::string{keyId});

        // If it still cannot be found, return an error.
        if (it == currentValidators->end()) {
            return {ErrorCodes::NoSuchKey, str::stream() << "Unknown key '" << keyId << "'"};
        }
    }
    return it->second;
}

/**
 * Helper function to load and validate an RSA key and return a key ID
 */
std::string JWKManager::_loadAndValidateRSAKey(const JWKRSA& RSAkey) {
    // Sanity check so that we don't load a dangerously small key.
    auto N = reduceInt(RSAkey.getModulus());
    uassert(ErrorCodes::BadValue,
            str::stream() << "Key scale is smaller (" << (N.size() << 3)
                          << " bits) than minimum required: " << (kMinKeySizeBytes << 3),
            N.size() >= kMinKeySizeBytes);

    // Sanity check so that we don't load an insensible encrypt component.
    auto E = reduceInt(RSAkey.getPublicExponent());
    uassert(ErrorCodes::BadValue,
            str::stream() << "Public key component invalid: "
                          << base64url::encode(RSAkey.getPublicExponent()),
            (E.size() > 1) || ((E.size() == 1) && (E[0] >= 3)));

    return std::string{RSAkey.getKeyId()};
}

/**
 * Helper function to load and validate an EC key and return a key ID
 */
std::string JWKManager::_loadAndValidateECKey(const JWKEC& ECkey) {
    // Sanity check to ensure the x and y coordinates are a minimum value
    // For P-256, length of X and Y coordinates is 32 bytes
    // For P-384, length of X and Y coordinates is 48 bytes
    size_t coordinateSize = [&ECkey]() {
        if (ECkey.getCurve() == "P-256"sv) {
            return 32;
        } else if (ECkey.getCurve() == "P-384"sv) {
            return 48;
        }
        uasserted(10858402, "Unsupported curve in fetched JWKSet");
    }();
    uassert(ErrorCodes::BadValue,
            str::stream() << "X-coordinate of EC component invalid: "
                          << base64url::encode(ECkey.getXcoordinate()),
            (ECkey.getXcoordinate().size() == coordinateSize));
    uassert(ErrorCodes::BadValue,
            str::stream() << "Y-coordinate of EC component invalid: "
                          << base64url::encode(ECkey.getYcoordinate()),
            (ECkey.getYcoordinate().size() == coordinateSize));
    return std::string{ECkey.getKeyId()};
}

Status JWKManager::loadKeys() try {
    bool isQuiesced = [this]() {
        auto* clockSource = _fetcher->getClockSource();
        return clockSource->now() <
            (getLastAttemptedFetchTime() + Seconds(gJWKSMinimumQuiescePeriodSecs.load()));
    }();
    if (isQuiesced) {
        return {ErrorCodes::OperationFailed, "Skipping refresh due to IdP quiesce"};
    }

    auto newValidators = std::make_shared<SharedValidatorMap>();
    auto newKeyMaterial = std::make_shared<KeyMap>();

    const auto& parsedKeys = _fetcher->fetch();
    for (const auto& key : parsedKeys.getKeys()) {
        auto JWK = JWK::parse(key, IDLParserContext("JWK"));

        if ((JWK.getType() != "RSA"sv) && (JWK.getType() != "EC")) {
            LOGV2_WARNING(
                8733001, "Unsupported key type in fetched JWK Set", "type"_attr = JWK.getType());
            continue;
        }
        uassert(ErrorCodes::BadValue, "Key ID must be non-empty", !JWK.getKeyId().empty());

        std::string keyId;
        if (JWK.getType() == "RSA") {
            keyId = _loadAndValidateRSAKey(JWKRSA::parse(key, IDLParserContext("JWKRSA")));
        } else if (JWK.getType() == "EC") {
            keyId = _loadAndValidateECKey(JWKEC::parse(key, IDLParserContext("JWKEC")));
        }
        uassert(ErrorCodes::DuplicateKeyId,
                str::stream() << "Key IDs must be unique, duplicate '" << keyId << "'",
                newKeyMaterial->find(keyId) == newKeyMaterial->end());

        newKeyMaterial->insert({keyId, key.copy()});

        auto swValidator = JWSValidator::create(JWK.getType(), key);
        uassertStatusOK(swValidator.getStatus());
        SharedValidator shValidator = std::move(swValidator.getValue());

        newValidators->insert({keyId, shValidator});
        LOGV2_DEBUG(7070202, 3, "Loaded JWK key", "kid"_attr = keyId, "typ"_attr = JWK.getType());
    }
    // We compare the old keys from the new ones and return if any old key has been removed or
    // modified in the new set of keys.
    _isKeyModified |= _haveKeysBeenModified(*newKeyMaterial);

    // A mutex is not used here because no single thread consumes both _validators and _keyMaterial.
    // _keyMaterial is used purely for reflection of current key state while _validators is used
    // for actual signature verification with the keys. Therefore, it's safe to update each
    // atomically rather than both under a mutex.
    std::atomic_exchange(&_validators, std::move(newValidators));    // NOLINT
    std::atomic_exchange(&_keyMaterial, std::move(newKeyMaterial));  // NOLINT

    return Status::OK();
} catch (const DBException& ex) {
    LOGV2_DEBUG(
        7938402, 3, "Failed loading JWKManager with keys", "error"_attr = ex.toStatus().reason());
    return ex.toStatus();
}

void JWKManager::serialize(BSONObjBuilder* bob) const {
    std::vector<BSONObj> keyVector;
    keyVector.reserve(size());

    auto currentKeys = std::atomic_load(&_keyMaterial);  // NOLINT
    std::transform(currentKeys->begin(),
                   currentKeys->end(),
                   std::back_inserter(keyVector),
                   [](const auto& keyEntry) { return keyEntry.second; });

    JWKSet jwks(keyVector);
    jwks.serialize(bob);
}

bool JWKManager::_haveKeysBeenModified(const KeyMap& newKeyMaterial) const {
    auto currentKeyMaterial = std::atomic_load(&_keyMaterial);  // NOLINT
    if (!currentKeyMaterial) {
        return false;
    }
    const bool hasBeenModified = std::any_of(
        (*currentKeyMaterial).cbegin(), (*currentKeyMaterial).cend(), [&](const auto& entry) {
            auto newKey = newKeyMaterial.find(entry.first);
            if (newKey == newKeyMaterial.end()) {
                // Key no longer exists in this JWKS.
                return true;
            }

            // Key still exists.
            return entry.second.woCompare(newKey->second) != 0;
        });

    return hasBeenModified;
}

}  // namespace mongo::crypto

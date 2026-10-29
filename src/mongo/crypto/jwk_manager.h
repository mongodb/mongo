// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/jwks_fetcher.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>


namespace mongo::crypto {

class [[MONGO_MOD_PUBLIC]] JWKManager {
public:
    using SharedValidator = std::shared_ptr<JWSValidator>;
    using KeyMap = std::map<std::string, BSONObj>;

    explicit JWKManager(std::unique_ptr<JWKSFetcher> fetcher);

    /**
     * Fetch a specific JWSValidator from the JWKManager by keyId.
     * If the keyId does not exist, it will refresh _keyMaterial and _validators and retry.
     * If it still cannot be found, it will return an error.
     */
    StatusWith<SharedValidator> getValidator(std::string_view keyId);

    std::size_t size() const {
        return _validators->size();
    }

    /**
     * Fetches a JWKS file for the specified Issuer URL using _fetcher, parses them as keys,
     * and instantiates JWSValidator instances. If the fetch fails or the parsed keys are invalid,
     * it leaves the validators and keyMaterial as-is and returns an error Status.
     */
    Status loadKeys();

    /**
     * Get current keys.
     */
    const KeyMap& getKeys() const {
        return *_keyMaterial;
    }

    bool getIsKeyModified() const {
        return _isKeyModified;
    }

    Date_t getLastAttemptedFetchTime() const {
        return _fetcher->getLastAttemptedFetchTime();
    }

    /**
     * Serialize the JWKs stored in this key manager into the BSONObjBuilder.
     */
    void serialize(BSONObjBuilder* bob) const;

private:
    std::unique_ptr<JWKSFetcher> _fetcher;

    // Stores the current key material of the manager, which may have been refreshed.
    std::shared_ptr<KeyMap> _keyMaterial;

    std::shared_ptr<std::map<std::string, SharedValidator>> _validators;

    // If an existing key got deleted or modified while doing a just in time refresh, we activate
    // this flag to indicate that a refresh occurred during this JWKManager's lifetime.
    bool _isKeyModified;

private:
    /**
     * Helper function to check whether a just-in-time refresh caused keys to be
     * modified after JWKManager initialization.
     */
    bool _haveKeysBeenModified(const KeyMap& newKeyMaterial) const;
    /**
     * Helper function to load and validate an RSA key and return a key ID
     */
    std::string _loadAndValidateRSAKey(const JWKRSA& RSAkey);

    /**
     * Helper function to load and validate an EC key and return a key ID
     */
    std::string _loadAndValidateECKey(const JWKEC& ECkey);
};

}  // namespace mongo::crypto

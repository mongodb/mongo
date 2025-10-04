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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/jwks_fetcher.h"
#include "mongo/crypto/jws_validator.h"
#include "mongo/crypto/jwt_types_gen.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>


namespace mongo::crypto {

class JWKManager {
public:
    using SharedValidator = std::shared_ptr<JWSValidator>;
    using KeyMap = std::map<std::string, BSONObj>;

    explicit JWKManager(std::unique_ptr<JWKSFetcher> fetcher);

    /**
     * Fetch a specific JWSValidator from the JWKManager by keyId.
     * If the keyId does not exist, it will refresh _keyMaterial and _validators and retry.
     * If it still cannot be found, it will return an error.
     */
    StatusWith<SharedValidator> getValidator(StringData keyId);

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

    /**
     * Serialize the JWKs stored in this key manager into the BSONObjBuilder.
     */
    void serialize(BSONObjBuilder* bob) const;

    /**
     * Returns TRUE if a fetch to IDP SHOULD NOT be performed at this time.
     * e.g. If a fetch was performed too recently.
     */
    bool quiesce() const {
        return _fetcher->quiesce();
    }

    /**
     * Sets a date to be used as the latest time a fetch happened.
     */
    void setQuiesce(Date_t quiesce) {
        _fetcher->setQuiesce(quiesce);
    }

private:
    bool _haveKeysBeenModified(const KeyMap& newKeyMaterial) const;

    std::unique_ptr<JWKSFetcher> _fetcher;

    // Stores the current key material of the manager, which may have been refreshed.
    std::shared_ptr<KeyMap> _keyMaterial;

    std::shared_ptr<std::map<std::string, SharedValidator>> _validators;

    // If an existing key got deleted or modified while doing a just in time refresh, we activate
    // this flag to indicate that a refresh occurred during this JWKManager's lifetime.
    bool _isKeyModified;

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

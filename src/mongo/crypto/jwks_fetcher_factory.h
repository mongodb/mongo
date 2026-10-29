// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/jwks_fetcher.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/** A Factory which creates objects needed to interact with Identity Providers.
 */

class [[MONGO_MOD_OPEN]] JWKSFetcherFactory {
public:
    virtual ~JWKSFetcherFactory() = default;
    virtual std::unique_ptr<crypto::JWKSFetcher> makeJWKSFetcher(std::string_view issuer) const = 0;
};


}  // namespace mongo

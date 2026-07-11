// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
class ClockSource;
namespace crypto {

/** Interface for objects which acquire JWKSes.
 */
class [[MONGO_MOD_PUBLIC]] JWKSFetcher {
public:
    JWKSFetcher() = default;
    virtual ~JWKSFetcher() = default;

    /**
     * Fetch the JWK set.
     */
    virtual JWKSet fetch() = 0;

    /**
     * Get the most recent fetch attempt time.
     */
    virtual Date_t getLastAttemptedFetchTime() const = 0;

    /**
     * Retrieves the ClockSource used to compute the lastAttemptedFetchTime.
     */
    virtual ClockSource* getClockSource() const = 0;
};

}  // namespace crypto
}  // namespace mongo

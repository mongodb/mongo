// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/jwks_fetcher.h"
#include "mongo/crypto/jwt_types_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <string>
#include <string_view>

namespace mongo {
namespace crypto {

/** JWKSFetcher implementation which acquires keys via HTTP.
 */
class [[MONGO_MOD_PUBLIC]] JWKSFetcherImpl : public JWKSFetcher {
public:
    JWKSFetcherImpl(ClockSource* clock, std::string_view issuer);

    JWKSet fetch() override;

    Date_t getLastAttemptedFetchTime() const override {
        return _lastAttemptedFetchTime.get();
    }

    ClockSource* getClockSource() const override {
        return _clock;
    }

protected:
    std::string _issuer;
    ClockSource* _clock;
    synchronized_value<Date_t> _lastAttemptedFetchTime;
};

}  // namespace crypto
}  // namespace mongo

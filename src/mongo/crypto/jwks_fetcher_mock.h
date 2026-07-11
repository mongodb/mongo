// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/jwks_fetcher_impl.h"
#include "mongo/util/clock_source.h"

#include <string_view>

namespace mongo::crypto {
using namespace std::literals::string_view_literals;

/**
 * Mock JWKS fetcher.
 * Returns JWKSes which are pre-set at construction.
 * Replies on JWKSFetcherImpl for other implementation details like quiesce mode.
 */
class MockJWKSFetcher : public JWKSFetcherImpl {
public:
    static constexpr std::string_view kMockIssuer = "https://localhost/issuer/mock"sv;

    MockJWKSFetcher(ClockSource* clock, BSONObj keys)
        : JWKSFetcherImpl(clock, kMockIssuer), _keys(std::move(keys)) {}

    JWKSet fetch() override {
        _lastAttemptedFetchTime = _clock->now();
        if (_shouldFail) {
            uasserted(ErrorCodes::NetworkTimeout,
                      "Mock JWKS fetcher configured to simulate timeout");
        }

        auto jwkSet = JWKSet::parse(_keys, IDLParserContext("JWKSet"));
        return jwkSet;
    }

    void setShouldFail(bool shouldFail) {
        _shouldFail = shouldFail;
    }

    void setKeys(BSONObj keys) {
        _keys = keys;
    }

private:
    BSONObj _keys;
    bool _shouldFail{false};
};

}  // namespace mongo::crypto

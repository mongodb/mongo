// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jwks_fetcher_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>

namespace mongo::crypto::test {
using namespace std::literals::string_view_literals;

class JWKManagerTest : public unittest::Test {
public:
    void setUp() override {
        _clock = std::make_unique<ClockSourceMock>();
        auto uniqueFetcher =
            std::make_unique<MockJWKSFetcher>(_clock.get(), BSON("keys"sv << BSONArray()));
        _jwksFetcher = uniqueFetcher.get();
        _jwkManager = std::make_unique<JWKManager>(std::move(uniqueFetcher));
    }

    ClockSourceMock* getClock() {
        return _clock.get();
    }

    JWKManager* jwkManager() {
        return _jwkManager.get();
    }

    MockJWKSFetcher* jwksFetcher() {
        return _jwksFetcher;
    }

private:
    std::unique_ptr<ClockSourceMock> _clock;
    MockJWKSFetcher* _jwksFetcher{nullptr};
    std::unique_ptr<JWKManager> _jwkManager;
};

}  // namespace mongo::crypto::test

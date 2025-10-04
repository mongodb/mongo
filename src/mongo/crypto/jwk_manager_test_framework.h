/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/jwk_manager.h"
#include "mongo/crypto/jwks_fetcher_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>

namespace mongo::crypto::test {

class JWKManagerTest : public unittest::Test {
public:
    void setUp() override {
        _clock = std::make_unique<ClockSourceMock>();
        auto uniqueFetcher =
            std::make_unique<MockJWKSFetcher>(_clock.get(), BSON("keys"_sd << BSONArray()));
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

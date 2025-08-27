/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/crypto/jwks_fetcher_impl.h"
#include "mongo/util/clock_source.h"

namespace mongo::crypto {

/**
 * Mock JWKS fetcher.
 * Returns JWKSes which are pre-set at construction.
 * Replies on JWKSFetcherImpl for other implementation details like quiesce mode.
 */
class MockJWKSFetcher : public JWKSFetcherImpl {
public:
    static constexpr StringData kMockIssuer = "https://localhost/issuer/mock"_sd;

    MockJWKSFetcher(ClockSource* clock, BSONObj keys)
        : JWKSFetcherImpl(clock, kMockIssuer), _keys(std::move(keys)) {}

    JWKSet fetch() override {
        if (_shouldFail) {
            uasserted(ErrorCodes::NetworkTimeout,
                      "Mock JWKS fetcher configured to simulate timeout");
        }

        auto jwkSet = JWKSet::parse(_keys, IDLParserContext("JWKSet"));
        _lastFetchQuiesceTime = _clock->now();
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

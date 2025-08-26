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

#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

// Custom AuthorizationRouterImpl which keeps track of how many times user cache invalidation
// functions have been called.
class AuthorizationRouterImplForTest : public AuthorizationRouterImpl {
public:
    using AuthorizationRouterImpl::AuthorizationRouterImpl;

    void invalidateUserByName(const UserName& user) override {
        _byNameCount.fetchAndAdd(1);
        AuthorizationRouterImpl::invalidateUserByName(user);
    }

    void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) override {
        _byTenantCount.fetchAndAdd(1);
        AuthorizationRouterImpl::invalidateUsersByTenant(tenant);
    }

    void invalidateUserCache() override {
        _wholeCacheCount.fetchAndAdd(1);
        AuthorizationRouterImpl::invalidateUserCache();
    }

    void resetCounts() {
        _byNameCount.store(0);
        _byTenantCount.store(0);
        _wholeCacheCount.store(0);
    }

    void assertCounts(uint64_t whole, uint64_t name, uint64_t tenant) {
        ASSERT_EQ(whole, _wholeCacheCount.load());
        ASSERT_EQ(name, _byNameCount.load());
        ASSERT_EQ(tenant, _byTenantCount.load());
    }

private:
    AtomicWord<uint64_t> _byNameCount = 0, _byTenantCount = 0, _wholeCacheCount = 0;
};

}  // namespace mongo

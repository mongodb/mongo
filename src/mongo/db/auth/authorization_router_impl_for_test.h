// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

namespace mongo {

// Custom AuthorizationRouterImpl which keeps track of how many times user cache invalidation
// functions have been called.
class AuthorizationRouterImplForTest : public AuthorizationRouterImpl {
public:
    struct Counts {
        uint64_t byName, byTenant, wholeCache;
    };

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

    Counts counts() const {
        return {_byNameCount.load(), _byTenantCount.load(), _wholeCacheCount.load()};
    }

private:
    Atomic<uint64_t> _byNameCount = 0, _byTenantCount = 0, _wholeCacheCount = 0;
};

}  // namespace mongo

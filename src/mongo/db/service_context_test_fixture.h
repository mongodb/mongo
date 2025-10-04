/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace service_context_test {

/**
 * "Literal" and "structural" type to stand-in for a `ClusterRole` value.
 * Necessary until `ClusterRole` can be used as a NTTP.
 */
enum class ServerRoleIndex { replicaSet, shard, router };

inline ClusterRole getClusterRole(ServerRoleIndex i) {
    switch (i) {
        case ServerRoleIndex::replicaSet:
            return ClusterRole::None;
        case ServerRoleIndex::shard:
            return ClusterRole::ShardServer;
        case ServerRoleIndex::router:
            return ClusterRole::RouterServer;
    }
    MONGO_UNREACHABLE;
}

/**
 * Virtual base for tests that need to set SC role before the
 * ServiceContextTest constructor. Necessary since ServiceContextTest
 * is often a virtual base and those run before all non-virtual bases.
 */
template <ServerRoleIndex roleIndex>
class RoleOverride {
public:
    ~RoleOverride() {
        serverGlobalParams.clusterRole = _saved;
    }

private:
    ClusterRole _saved{std::exchange(serverGlobalParams.clusterRole, getClusterRole(roleIndex))};
};

using ReplicaSetRoleOverride = RoleOverride<ServerRoleIndex::replicaSet>;
using ShardRoleOverride = RoleOverride<ServerRoleIndex::shard>;
using RouterRoleOverride = RoleOverride<ServerRoleIndex::router>;

/**
 * A hook for the ServiceContextTest that is used configure whether or not an egress-only
 * TransportLayer is setup. This is an implementation detail and should only be accessed through
 * WithoutSetupTransportLayer or WithSetupTransportLayer.
 *
 * Accessing `shouldSetupTL` (or any base class member) in a delegating constructor is undefined
 * behavior.
 *
 * Ex:
 *      class Foo : WithSetupTransportLayer, public ServiceContextTest {...};
 *
 *      The first constructor to get executed is the default WithSetupTransportLayer constructor
 *      which sets shouldSetupTL to true. Next we construct the ServiceContextTest which begins with
 *      the WithoutSetupTransportLayer default constructor. This default constructor does not set
 *      shouldSetupTL or use the default value of false - it retains the same value that was set in
 *      WithSetupTransportLayer thanks to virtual inheritance. Finally, when we run the
 *      ServiceContextTest constructor, accessing the shouldSetupTL field will yield true.
 */
struct ServiceContextTestHook {
    bool shouldSetupTL = false;
};

/**
 * This class will either use the default value of shouldSetupTL or the value set by another object
 * that virtually inherites from ServiceContextTestHook (see example in ServiceContextTestHook).
 */
class WithoutSetupTransportLayer : public virtual ServiceContextTestHook {};

/**
 * Configures a ServiceContextTest to setup a TransportLayer to be stored on the ServiceContext. It
 * uses virtual inheritance to guarantee that there is only one value of shouldSetupTL and that this
 * value is set up before the ServiceContextTest constructor. It must precede ServiceContextTest (or
 * any of its derived classes) in the base class list.
 */
class WithSetupTransportLayer : virtual ServiceContextTestHook {
public:
    WithSetupTransportLayer() {
        shouldSetupTL = true;
    }
};

class ScopedGlobalServiceContextForTest {
public:
    ScopedGlobalServiceContextForTest();
    explicit ScopedGlobalServiceContextForTest(bool shouldSetupTL);
    explicit ScopedGlobalServiceContextForTest(ServiceContext::UniqueServiceContext serviceContext)
        : ScopedGlobalServiceContextForTest{std::move(serviceContext), false} {}
    ScopedGlobalServiceContextForTest(ServiceContext::UniqueServiceContext serviceContext,
                                      bool shouldSetupTL);
    virtual ~ScopedGlobalServiceContextForTest();

    /**
     * Returns a service context, which is only valid for this instance of the test.
     */
    ServiceContext* getServiceContext() const;

    Service* getService() const;
};

/**
 * Test fixture for tests that require a properly initialized global service context. Subclasses
 * that need to use a TransportLayer for egress should also derive from WithSetupTransportLayer.
 * WithSetupTransportLayer must precede ServiceContextTest in the list of base classess.
 */
class ServiceContextTest : public WithoutSetupTransportLayer, public unittest::Test {
public:
    /**
     * Returns the default Client for this test.
     */
    Client* getClient() {
        return Client::getCurrent();
    }

    ServiceContext::UniqueOperationContext makeOperationContext() {
        return getClient()->makeOperationContext();
    }

    ServiceContext* getServiceContext() const {
        return _scopedServiceContext->getServiceContext();
    }

    Service* getService() const {
        return _scopedServiceContext->getService();
    }

protected:
    ServiceContextTest();
    explicit ServiceContextTest(
        std::unique_ptr<ScopedGlobalServiceContextForTest> scopedServiceContext,
        std::shared_ptr<transport::Session> session = nullptr);
    ~ServiceContextTest() override;

    ScopedGlobalServiceContextForTest* scopedServiceContext() const {
        return _scopedServiceContext.get();
    }

private:
    std::unique_ptr<ScopedGlobalServiceContextForTest> _scopedServiceContext;
    ThreadClient _threadClient;
};

/**
 * Test fixture for tests that require a properly-initialized global service context with
 * the fast and precise clock sources set to instances of ClockSourceMock and the tick source
 * set to an instance of TickSourceMock.
 */
class ClockSourceMockServiceContextTest : public ServiceContextTest {
protected:
    ClockSourceMockServiceContextTest();
};

/**
 * Test fixture for tests that require a properly-initialized global service context with
 * the fast and precise clock sources set to instances of SharedClockSourceAdapter with the
 * same underlying clock source.
 */
class SharedClockSourceAdapterServiceContextTest : public ServiceContextTest {
protected:
    explicit SharedClockSourceAdapterServiceContextTest(std::shared_ptr<ClockSource> clock);
};

}  // namespace service_context_test

using service_context_test::ClockSourceMockServiceContextTest;
using service_context_test::ScopedGlobalServiceContextForTest;
using service_context_test::ServiceContextTest;
using service_context_test::SharedClockSourceAdapterServiceContextTest;

}  // namespace mongo

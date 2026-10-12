// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace service_context_test {

class [[MONGO_MOD_OPEN]] ScopedGlobalServiceContextForTest {
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
 * that need to use a TransportLayer for egress should override `shouldSetupTransportLayer()`.
 */
class [[MONGO_MOD_OPEN]] ServiceContextTest : public unittest::Test {
public:
    Client* getClient() {
        Client* client = Client::getCurrent();
        invariant(client, "The Client is not available until setUp() has run.");
        return client;
    }

    ServiceContext::UniqueOperationContext makeOperationContext() {
        return getClient()->makeOperationContext();
    }

    ServiceContext* getServiceContext() const {
        return scopedServiceContext()->getServiceContext();
    }

    Service* getService() const {
        return scopedServiceContext()->getService();
    }

    /** True once `setUp()` has built the ServiceContext. */
    bool hasServiceContext() const {
        return static_cast<bool>(_scopedServiceContext);
    }

    /** Builds the ScopedGlobalServiceContextForTest that `setUp()` installs. */
    virtual std::unique_ptr<ScopedGlobalServiceContextForTest> makeScopedServiceContext();

    /**
     * Whether an egress-only TransportLayer should be set up on the built ServiceContext.
     */
    virtual bool shouldSetupTransportLayer() const {
        return false;
    }

    /**
     * When overridden to return a role, `setUp()` sets `serverGlobalParams.clusterRole` to that
     * role before building the ServiceContext, and the destructor restores the prior value.
     */
    virtual boost::optional<ClusterRole> getClusterRoleOverride() const {
        return boost::none;
    }

    /**
     * The Session to attach the fixture's Client to.
     */
    virtual std::shared_ptr<transport::Session> makeSession() {
        return nullptr;
    }

    /**
     * Iniitializes and configures a ServiceContext, in part by invoking hooks to other virtual
     * functions. This initialization is not (and cannot be) done in the constructor.
     *
     * There's no matching `tearDown()` override. The `ServiceContext`-dependent data members are
     * initialized in `setUp`, and the destructor kills them.
     */
    void setUp() override;

protected:
    ServiceContextTest();
    ~ServiceContextTest() override;

    ScopedGlobalServiceContextForTest* scopedServiceContext() const {
        invariant(_scopedServiceContext, "ServiceContext is unavailable before setUp().");
        return _scopedServiceContext.get();
    }

private:
    std::unique_ptr<ScopedGlobalServiceContextForTest> _scopedServiceContext;
    boost::optional<ThreadClient> _threadClient;
    boost::optional<ClusterRole> _savedClusterRole;
};

/**
 * Test fixture for tests that require a properly-initialized global service context with
 * the fast and precise clock sources set to instances of ClockSourceMock and the tick source
 * set to an instance of TickSourceMock.
 */
class [[MONGO_MOD_OPEN]] ClockSourceMockServiceContextTest : public ServiceContextTest {
protected:
    ClockSourceMockServiceContextTest();

    std::unique_ptr<ScopedGlobalServiceContextForTest> makeScopedServiceContext() override;
};

/**
 * Test fixture for tests that require a properly-initialized global service context with
 * the fast and precise clock sources set to instances of SharedClockSourceAdapter with the
 * same underlying clock source.
 */
class [[MONGO_MOD_OPEN]] SharedClockSourceAdapterServiceContextTest : public ServiceContextTest {
protected:
    explicit SharedClockSourceAdapterServiceContextTest(std::shared_ptr<ClockSource> clock);

    std::unique_ptr<ScopedGlobalServiceContextForTest> makeScopedServiceContext() override;

private:
    std::shared_ptr<ClockSource> _clock;
};

}  // namespace service_context_test

using service_context_test::ClockSourceMockServiceContextTest;
using service_context_test::ScopedGlobalServiceContextForTest;
using service_context_test::ServiceContextTest;
using service_context_test::SharedClockSourceAdapterServiceContextTest;

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/util/modules.h"

namespace mongo {

class SessionCatalogTest : public ServiceContextTest {
protected:
    SessionCatalog* catalog();
    void assertCanCheckoutSession(const LogicalSessionId& lsid);
    void assertSessionCheckoutTimesOut(const LogicalSessionId& lsid);
    void assertConcurrentCheckoutTimesOut(const LogicalSessionId& lsid);

    /**
     * Creates the session with the given 'lsid' by checking it out from the SessionCatalog and then
     * checking it back in.
     */
    void createSession(const LogicalSessionId& lsid);

    /**
     * Returns the session ids for all sessions in the SessionCatalog.
     */
    std::vector<LogicalSessionId> getAllSessionIds(OperationContext* opCtx);
};

class SessionCatalogTestWithDefaultOpCtx : public SessionCatalogTest {
protected:
    const ServiceContext::UniqueOperationContext _uniqueOpCtx = makeOperationContext();
    OperationContext* const _opCtx = _uniqueOpCtx.get();
};

// When this class is in scope, makes the system behave as if we're in a DBDirectClient
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(_opCtx->getClient()->isInDirectClient()) {
        _opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};
}  // namespace mongo

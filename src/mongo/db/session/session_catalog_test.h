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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/session_catalog.h"

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

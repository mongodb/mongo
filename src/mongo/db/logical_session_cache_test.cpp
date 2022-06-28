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

#include "mongo/platform/basic.h"

#include "mongo/db/logical_session_cache_impl.h"

#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_liaison_mock.h"
#include "mongo/db/sessions_collection_mock.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const Milliseconds kSessionTimeout = duration_cast<Milliseconds>(kLogicalSessionDefaultTimeout);
const Milliseconds kForceRefresh{kLogicalSessionRefreshMillisDefault};

using SessionList = std::list<LogicalSessionId>;
using unittest::EnsureFCV;

/**
 * Test fixture that sets up a session cache attached to a mock service liaison
 * and mock sessions collection implementation.
 */
class LogicalSessionCacheTest : public ServiceContextTest {
public:
    LogicalSessionCacheTest()
        : _service(std::make_shared<MockServiceLiaisonImpl>()),
          _sessions(std::make_shared<MockSessionsCollectionImpl>()) {

        AuthorizationManager::set(getServiceContext(),
                                  AuthorizationManager::create(getServiceContext()));

        // Re-initialize the client after setting the AuthorizationManager to get an
        // AuthorizationSession.
        Client::releaseCurrent();
        Client::initThread(getThreadName());
        _opCtx = makeOperationContext();

        auto mockService = std::make_unique<MockServiceLiaison>(_service);
        auto mockSessions = std::make_unique<MockSessionsCollection>(_sessions);
        _cache = std::make_unique<LogicalSessionCacheImpl>(
            std::move(mockService),
            std::move(mockSessions),
            [](OperationContext*, SessionsCollection&, Date_t) {
                return 0; /* No op*/
            });
    }

    void waitUntilRefreshScheduled() {
        while (service()->jobs() < 2) {
            sleepmillis(10);
        }
    }

    std::unique_ptr<LogicalSessionCache>& cache() {
        return _cache;
    }

    std::shared_ptr<MockServiceLiaisonImpl> service() {
        return _service;
    }

    std::shared_ptr<MockSessionsCollectionImpl> sessions() {
        return _sessions;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;

    std::shared_ptr<MockServiceLiaisonImpl> _service;
    std::shared_ptr<MockSessionsCollectionImpl> _sessions;

    std::unique_ptr<LogicalSessionCache> _cache;
};

TEST_F(LogicalSessionCacheTest, ParentAndChildSessionsHaveEqualLogicalSessionRecord) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto lastUse = Date_t::now();
    auto parentSessionRecord = makeLogicalSessionRecord(parentLsid, lastUse);

    auto childSessionRecord0 = makeLogicalSessionRecord(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid), lastUse);
    ASSERT_BSONOBJ_EQ(parentSessionRecord.toBSON(), childSessionRecord0.toBSON());

    auto childSessionRecord1 =
        makeLogicalSessionRecord(makeLogicalSessionIdWithTxnUUIDForTest(parentLsid), lastUse);
    ASSERT_BSONOBJ_EQ(parentSessionRecord.toBSON(), childSessionRecord1.toBSON());
}

// Test that promoting from the cache updates the lastUse date of records
TEST_F(LogicalSessionCacheTest, VivifyUpdatesLastUse) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        auto start = service()->now();

        // Insert the record into the sessions collection with 'start'
        ASSERT_OK(cache()->startSession(opCtx(), makeLogicalSessionRecord(lsid, start)));

        // Fast forward time and promote
        service()->fastForward(Milliseconds(500));
        ASSERT_OK(cache()->vivify(opCtx(), lsid));

        // Now that we promoted, lifetime of session should be extended
        service()->fastForward(kSessionTimeout - Milliseconds(500));
        ASSERT_OK(cache()->vivify(opCtx(), lsid));

        // We promoted again, so lifetime extended again
        service()->fastForward(kSessionTimeout - Milliseconds(500));
        ASSERT_OK(cache()->vivify(opCtx(), lsid));

        // Fast forward and promote
        service()->fastForward(kSessionTimeout - Milliseconds(10));
        ASSERT_OK(cache()->vivify(opCtx(), lsid));

        // Lifetime extended again
        service()->fastForward(Milliseconds(11));
        ASSERT_OK(cache()->vivify(opCtx(), lsid));
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(LogicalSessionCacheTest, VivifyUpdatesLastUseOfParentSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        ASSERT_OK(
            cache()->startSession(opCtx(), makeLogicalSessionRecord(parentLsid, service()->now())));
        service()->fastForward(Minutes(1));
        ASSERT_OK(cache()->vivify(opCtx(), childLsid));
        ASSERT_OK(cache()->refreshNow(opCtx()));

        auto records = sessions()->sessions();
        ASSERT_EQ(1, records.size());
        ASSERT_EQ(service()->now(), records.begin()->second.getLastUse());
        sessions()->clearSessions();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid),
            makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

// Test the startSession method
TEST_F(LogicalSessionCacheTest, StartSession) {
    auto runTest = [&](const LogicalSessionId& lsid0, const LogicalSessionId& lsid1) {
        auto parentLsid0 = getParentSessionId(lsid0);
        auto record = makeLogicalSessionRecord(lsid0, service()->now());

        // Test starting a new session
        ASSERT_OK(cache()->startSession(opCtx(), record));

        // Record will not be in the collection yet; refresh must happen first.
        ASSERT(!sessions()->has(lsid0));

        // Do refresh, cached records should get flushed to collection.
        ASSERT(cache()->refreshNow(opCtx()).isOK());
        if (parentLsid0) {
            ASSERT(!sessions()->has(lsid0));
            ASSERT(sessions()->has(*parentLsid0));
        } else {
            ASSERT(sessions()->has(lsid0));
        }

        // Try to start the same session again, should succeed.
        ASSERT_OK(cache()->startSession(opCtx(), record));

        // Try to start a session that is already in the sessions collection but
        // is not in our local cache, should succeed.
        auto record1 = makeLogicalSessionRecord(lsid1, service()->now());
        sessions()->add(record1);
        ASSERT_OK(cache()->startSession(opCtx(), record1));

        // Try to start a session that has expired from our cache, and is no
        // longer in the sessions collection, should succeed
        service()->fastForward(Milliseconds(kSessionTimeout.count() + 5));
        sessions()->remove(parentLsid0 ? *parentLsid0 : lsid0);
        if (parentLsid0) {
            ASSERT(!sessions()->has(lsid0));
            ASSERT(!sessions()->has(*parentLsid0));
        } else {
            ASSERT(!sessions()->has(lsid0));
        }
        ASSERT_OK(cache()->startSession(opCtx(), record));
    };

    runTest(makeLogicalSessionIdForTest(), makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(),
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest(), makeLogicalSessionIdWithTxnUUIDForTest());
}

// Test the endSessions method.
TEST_F(LogicalSessionCacheTest, EndSessions) {
    const auto lsids = []() -> std::vector<LogicalSessionId> {
        auto lsid0 = makeLogicalSessionIdForTest();
        auto lsid1 = makeLogicalSessionIdForTest();
        auto lsid2 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(lsid1);
        auto lsid3 = makeLogicalSessionIdWithTxnUUIDForTest(lsid1);
        return {lsid0, lsid1, lsid2, lsid3};
    }();

    for (const auto& lsid : lsids) {
        ASSERT_OK(cache()->startSession(opCtx(), makeLogicalSessionRecord(lsid, service()->now())));
    }
    ASSERT_EQ(2UL, cache()->size());

    // Verify that it is invalid to pass an lsid with a parent lsid into endSessions.
    ASSERT_THROWS_CODE(cache()->endSessions({lsids[2]}), DBException, ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(cache()->endSessions({lsids[3]}), DBException, ErrorCodes::InvalidOptions);

    cache()->endSessions({lsids[0], lsids[1]});
}

// Test the peekCached method.
TEST_F(LogicalSessionCacheTest, PeekCached) {
    auto lsid0 = makeLogicalSessionIdForTest();
    auto record0 = makeLogicalSessionRecord(lsid0, service()->now());
    ASSERT_OK(cache()->startSession(opCtx(), record0));
    ASSERT_BSONOBJ_EQ(record0.toBSON(), cache()->peekCached(lsid0)->toBSON());

    // Verify that it is invalid to pass an lsid with a parent lsid into peekCached.
    auto lsid1 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(lsid0);
    ASSERT_THROWS_CODE(cache()->peekCached(lsid1), DBException, ErrorCodes::InvalidOptions);
    auto lsid2 = makeLogicalSessionIdWithTxnUUIDForTest(lsid0);
    ASSERT_THROWS_CODE(cache()->peekCached(lsid2), DBException, ErrorCodes::InvalidOptions);
}

// Test that session cache properly expires lsids after 30 minutes of no use
TEST_F(LogicalSessionCacheTest, BasicSessionExpiration) {
    // Insert a lsid
    auto record = makeLogicalSessionRecordForTest();
    ASSERT_OK(cache()->startSession(opCtx(), record));
    ASSERT_EQ(1UL, cache()->size());

    // Force it to expire
    service()->fastForward(Milliseconds(kSessionTimeout.count() + 5));

    // Check that it is no longer in the cache
    ASSERT_OK(cache()->refreshNow(opCtx()));
    ASSERT_EQ(0UL, cache()->size());
}

// Test large sets of cache-only session lsids
TEST_F(LogicalSessionCacheTest, ManySignedLsidsInCacheRefresh) {
    int count = 10000;
    for (int i = 0; i < count; i++) {
        auto record = makeLogicalSessionRecordForTest();
        ASSERT_OK(cache()->startSession(opCtx(), record));
    }

    // Check that all signedLsids refresh
    sessions()->setRefreshHook([&count](const LogicalSessionRecordSet& sessions) {
        ASSERT_EQ(sessions.size(), size_t(count));
        return Status::OK();
    });

    // Force a refresh
    service()->fastForward(kForceRefresh);
    ASSERT_OK(cache()->refreshNow(opCtx()));
}

//
TEST_F(LogicalSessionCacheTest, RefreshMatrixSessionState) {
    const std::vector<std::vector<std::string>> stateNames = {
        {"active", "inactive"},
        {"running", "not running"},
        {"expired", "unexpired"},
        {"ended", "not ended"},
        {"cursor", "no cursor"},
    };
    struct {
        // results that we test for after the _refresh
        bool inCollection;
        bool killed;
    } testCases[] = {
        // 0, active, running, expired, ended, cursor
        {false, true},
        // 1, inactive, running, expired, ended, cursor
        {false, true},
        // 2, active, not running, expired, ended, cursor
        {false, true},
        // 3, inactive, not running, expired, ended, cursor
        {false, true},
        // 4, active, running, unexpired, ended, cursor
        {false, true},
        // 5, inactive, running, unexpired, ended, cursor
        {false, true},
        // 6, active, not running, unexpired, ended, cursor
        {false, true},
        // 7, inactive, not running, unexpired, ended, cursor
        {false, true},
        // 8, active, running, expired, not ended, cursor
        {true, false},
        // 9, inactive, running, expired, not ended, cursor
        {false, true},
        // 10, active, not running, expired, not ended, cursor
        {true, false},
        // 11, inactive, not running, expired, not ended, cursor
        {false, true},
        // 12, active, running, unexpired, not ended, cursor
        {true, false},
        // 13, inactive, running, unexpired, not ended, cursor
        {true, false},
        // 14, active, not running, unexpired, not ended, cursor
        {true, false},
        // 15, inactive, not running, unexpired, not ended, cursor
        {true, false},
        // 16, active, running, expired, ended, no cursor
        {false, true},
        // 17, inactive, running, expired, ended, no cursor
        {false, true},
        // 18, active, not running, expired, ended, no cursor
        {false, true},
        // 19, inactive, not running, expired, ended, no cursor
        {false, true},
        // 20, active, running, unexpired, ended, no cursor
        {false, true},
        // 21, inactive, running, unexpired, ended, no cursor
        {false, true},
        // 22, active, not running, unexpired, ended, no cursor
        {false, true},
        // 23, inactive, not running, unexpired, ended, no cursor
        {false, true},
        // 24, active, running, expired, not ended, no cursor
        {true, false},
        // 25, inactive, running, expired, not ended, no cursor
        {false, true},
        // 26, active, not running, expired, not ended, no cursor
        {true, false},
        // 27, inactive, not running, expired, not ended, no cursor
        {false, false},
        // 28, active, running, unexpired, not ended, no cursor
        {true, false},
        // 29, inactive, running, unexpired, not ended, no cursor
        {true, false},
        // 30, active, not running, unexpired, not ended, no cursor
        {true, false},
        // 31, inactive, not running, unexpired, not ended, no cursor
        {true, false},
    };

    std::vector<LogicalSessionId> ids;
    for (int i = 0; i < 32; i++) {

        bool active = !(i & 1);
        bool running = !(i & 2);
        bool expired = !(i & 4);
        bool ended = !(i & 8);
        bool cursor = !(i & 16);

        auto lsid = makeLogicalSessionIdForTest();
        ids.push_back(lsid);
        auto lsRecord = makeLogicalSessionRecord(lsid, service()->now() + Milliseconds(500));

        if (running) {
            service()->add(lsid);
        }
        if (active) {
            ASSERT_OK(cache()->startSession(opCtx(), lsRecord));
        }
        if (!expired) {
            sessions()->add(lsRecord);
        }
        if (ended) {
            LogicalSessionIdSet lsidSet;
            lsidSet.emplace(lsid);
            cache()->endSessions(lsidSet);
        }
        if (cursor) {
            service()->addCursorSession(lsid);
        }
    }

    // Force a refresh
    service()->fastForward(kForceRefresh);
    ASSERT_OK(cache()->refreshNow(opCtx()));

    for (int i = 0; i < 32; i++) {
        std::stringstream failText;
        failText << "case " << i << " : ";
        for (int j = 0; j < 4; j++) {
            failText << stateNames[j][i >> j & 1] << " ";
        }
        failText << " session case failed: ";

        ASSERT(sessions()->has(ids[i]) == testCases[i].inCollection)
            << failText.str()
            << (testCases[i].inCollection ? "session wasn't in collection"
                                          : "session was in collection");
        ASSERT((service()->matchKilled(ids[i]) != nullptr) == testCases[i].killed)
            << failText.str()
            << (testCases[i].killed ? "session wasn't killed" : "session was killed");
    }
}


}  // namespace
}  // namespace mongo

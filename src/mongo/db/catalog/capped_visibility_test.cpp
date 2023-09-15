/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/capped_visibility.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

struct ClientAndOpCtx {
    ClientAndOpCtx(ServiceContext* service, std::string desc)
        : client(service->makeClient(std::move(desc), nullptr)),
          opCtx(client->makeOperationContext()) {}

    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

class CappedVisibilityTest : public unittest::Test, public ScopedGlobalServiceContextForTest {};

TEST_F(CappedVisibilityTest, EmptySnapshotNoneVisible) {
    CappedVisibilityObserver observer("test");
    auto snapshot = observer.makeSnapshot();
    ASSERT_FALSE(snapshot.isRecordVisible(RecordId(1)));
}

TEST_F(CappedVisibilityTest, BasicRecordIdHole) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");

    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    auto writer2 = observer.registerWriter(cando2.opCtx->recoveryUnit());

    writer1->registerRecordId(RecordId(2));
    writer2->registerRecordId(RecordId(3));

    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    cando1.opCtx->recoveryUnit()->commitUnitOfWork();

    // All RecordIds should be visible now.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }
}

TEST_F(CappedVisibilityTest, RollBack) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    writer1->registerRecordId(RecordId(2));

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
    }

    cando1.opCtx->recoveryUnit()->abortUnitOfWork();

    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        // RecordId 2 was not committed, so it should not be considered visible.
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }
}

TEST_F(CappedVisibilityTest, RollBackHole) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");
    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    auto writer2 = observer.registerWriter(cando2.opCtx->recoveryUnit());

    writer1->registerRecordId(RecordId(2));
    writer2->registerRecordId(RecordId(3));
    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    cando1.opCtx->recoveryUnit()->abortUnitOfWork();

    // All committed RecordIds should be visible now.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        // Even though RecordId 2 was not committed, it should be considered visible.
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }
}

// Hole with multiple uncommitted writers and one writer hasn't register any records yet.
TEST_F(CappedVisibilityTest, UnregisteredRecords) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");
    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    auto writer2 = observer.registerWriter(cando2.opCtx->recoveryUnit());

    writer1->registerRecordId(RecordId(2));

    // The highest visible record should be 1
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    writer2->registerRecordId(RecordId(3));

    // The highest visible record should still be 1
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    cando1.opCtx->recoveryUnit()->commitUnitOfWork();

    // RecordIds except for 3 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // All RecordIds should be visible now.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }
}

TEST_F(CappedVisibilityTest, RegisterRange) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");
    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    auto writer2 = observer.registerWriter(cando2.opCtx->recoveryUnit());

    writer1->registerRecordIds(RecordId(2), RecordId(5));
    writer2->registerRecordIds(RecordId(6), RecordId(10));

    // The highest visible record should be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(6)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(10)));
    }

    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // The highest visible record should be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(6)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(10)));
    }

    cando1.opCtx->recoveryUnit()->commitUnitOfWork();

    // All records should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(6)));
        ASSERT(snapshot.isRecordVisible(RecordId(10)));
    }
}

TEST_F(CappedVisibilityTest, MultiRegistration) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");
    auto writer1 = observer.registerWriter(cando1.opCtx->recoveryUnit());
    auto writer2 = observer.registerWriter(cando2.opCtx->recoveryUnit());

    writer1->registerRecordId(RecordId(2));
    writer2->registerRecordId(RecordId(3));
    writer1->registerRecordId(RecordId(4));
    writer2->registerRecordId(RecordId(5));

    // The highest visible record should be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(5)));
    }

    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // The highest visible record should still be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(5)));
    }

    cando1.opCtx->recoveryUnit()->commitUnitOfWork();

    // All records should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT(snapshot.isRecordVisible(RecordId(4)));
        ASSERT(snapshot.isRecordVisible(RecordId(5)));
    }
}

class CappedCollection {
public:
    CappedCollection(StringData ident) : _observer(ident) {}

    void insertRecordImmediately(RecordId id) {
        _observer.setRecordImmediatelyVisible(id);
    }

    void insertRecord(RecoveryUnit* ru, RecordId id) {
        auto writer = _observer.registerWriter(ru);
        writer->registerRecordId(id);
    }

    CappedVisibilitySnapshot makeSnapshot() {
        return _observer.makeSnapshot();
    }

private:
    CappedVisibilityObserver _observer;
};

// Tests writes to multiple capped collections at once
TEST_F(CappedVisibilityTest, MultiCollection) {
    CappedCollection coll1("coll1");
    CappedCollection coll2("coll2");

    coll1.insertRecordImmediately(RecordId(1));
    coll2.insertRecordImmediately(RecordId(11));

    ClientAndOpCtx cando1(getServiceContext(), "Client1");
    ClientAndOpCtx cando2(getServiceContext(), "Client2");

    coll1.insertRecord(cando1.opCtx->recoveryUnit(), RecordId(2));
    coll1.insertRecord(cando2.opCtx->recoveryUnit(), RecordId(3));

    coll2.insertRecord(cando1.opCtx->recoveryUnit(), RecordId(12));
    coll2.insertRecord(cando2.opCtx->recoveryUnit(), RecordId(13));

    // Only the first record should be visible to both collections.
    {
        auto snapshot = coll1.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    {
        auto snapshot = coll2.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(11)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(12)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(13)));
    }

    cando2.opCtx->recoveryUnit()->commitUnitOfWork();

    // Nothing should become newly visible
    {
        auto snapshot = coll1.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    {
        auto snapshot = coll2.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(11)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(12)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(13)));
    }

    cando1.opCtx->recoveryUnit()->commitUnitOfWork();

    // All RecordIds should be visible now.
    {
        auto snapshot = coll1.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }

    {
        auto snapshot = coll2.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(11)));
        ASSERT(snapshot.isRecordVisible(RecordId(12)));
        ASSERT(snapshot.isRecordVisible(RecordId(13)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(14)));
    }
}

}  // namespace
}  // namespace mongo

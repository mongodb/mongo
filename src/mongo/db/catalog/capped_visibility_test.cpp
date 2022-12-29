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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/capped_visibility.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using OpCtxAndRecoveryUnit =
    std::pair<std::unique_ptr<OperationContext>, std::unique_ptr<RecoveryUnit>>;

OpCtxAndRecoveryUnit makeOpCtxAndRecoveryUnit() {
    auto opCtx = std::make_unique<OperationContextNoop>();
    auto ru = std::make_unique<RecoveryUnitNoop>();
    ru->setOperationContext(opCtx.get());
    return {std::move(opCtx), std::move(ru)};
}

// Basic RecordId hole
TEST(CappedVisibilityTest, BasicHole) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));


    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    auto writer2 = observer.registerWriter(ru2.get());

    writer1->registerRecordId(RecordId(2));
    writer2->registerRecordId(RecordId(3));
    ru2->commitUnitOfWork();

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    ru1->commitUnitOfWork();

    // All RecordIds should be visible now.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }
}

TEST(CappedVisibilityTest, RollBack) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    writer1->registerRecordId(RecordId(2));

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
    }

    ru1->abortUnitOfWork();

    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        // RecordId 2 was not committed, so it should not be considered visible.
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }
}

TEST(CappedVisibilityTest, RollBackHole) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    auto writer2 = observer.registerWriter(ru2.get());

    writer1->registerRecordId(RecordId(2));
    writer2->registerRecordId(RecordId(3));
    ru2->commitUnitOfWork();

    // Only RecordId 1 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    ru1->abortUnitOfWork();

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
TEST(CappedVisibilityTest, UnregisteredRecords) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    auto writer2 = observer.registerWriter(ru2.get());

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

    ru1->commitUnitOfWork();

    // RecordIds except for 3 should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
    }

    ru2->commitUnitOfWork();

    // All RecordIds should be visible now.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
    }
}

TEST(CappedVisibilityTest, RegisterRange) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    auto writer2 = observer.registerWriter(ru2.get());

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

    ru2->commitUnitOfWork();

    // The highest visible record should be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(6)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(10)));
    }

    ru1->commitUnitOfWork();
    // All records should be visible.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT(snapshot.isRecordVisible(RecordId(2)));
        ASSERT(snapshot.isRecordVisible(RecordId(6)));
        ASSERT(snapshot.isRecordVisible(RecordId(10)));
    }
}

TEST(CappedVisibilityTest, MultiRegistration) {
    CappedVisibilityObserver observer("test");
    observer.setRecordImmediatelyVisible(RecordId(1));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();
    auto writer1 = observer.registerWriter(ru1.get());
    auto writer2 = observer.registerWriter(ru2.get());

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

    ru2->commitUnitOfWork();

    // The highest visible record should still be 1.
    {
        auto snapshot = observer.makeSnapshot();
        ASSERT(snapshot.isRecordVisible(RecordId(1)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(2)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(3)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(4)));
        ASSERT_FALSE(snapshot.isRecordVisible(RecordId(5)));
    }

    ru1->commitUnitOfWork();

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
TEST(CappedVisibilityTest, MultiCollection) {
    CappedCollection coll1("coll1");
    CappedCollection coll2("coll2");

    coll1.insertRecordImmediately(RecordId(1));
    coll2.insertRecordImmediately(RecordId(11));

    auto [op1, ru1] = makeOpCtxAndRecoveryUnit();
    auto [op2, ru2] = makeOpCtxAndRecoveryUnit();

    coll1.insertRecord(ru1.get(), RecordId(2));
    coll1.insertRecord(ru2.get(), RecordId(3));

    coll2.insertRecord(ru1.get(), RecordId(12));
    coll2.insertRecord(ru2.get(), RecordId(13));

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

    ru2->commitUnitOfWork();

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

    ru1->commitUnitOfWork();

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

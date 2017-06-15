/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const std::string& suffix = "") {
    return NamespaceString(std::string("local." + t.getSuiteName() + "_" + t.getTestName())
                               .substr(0, NamespaceString::MaxNsCollectionLen - suffix.length()) +
                           suffix);
}

/**
 * Returns min valid document.
 */
BSONObj getMinValidDocument(OperationContext* opCtx, const NamespaceString& minValidNss) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::DBLock dblk(opCtx, minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(opCtx->lockState(), minValidNss.ns(), MODE_IS);
        BSONObj mv;
        if (Helpers::getSingleton(opCtx, minValidNss.ns().c_str(), mv)) {
            return mv;
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "getMinValidDocument", minValidNss.ns());
    return BSONObj();
}

class ReplicationConsistencyMarkersTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface* getStorageInterface() {
        return _storageInterface.get();
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        auto replCoord = stdx::make_unique<ReplicationCoordinatorMock>(getServiceContext());
        ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
        _storageInterface = stdx::make_unique<StorageInterfaceImpl>();
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _storageInterface.reset();
        ServiceContextMongoDTest::tearDown();
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<StorageInterfaceImpl> _storageInterface;
};

/**
 * Recovery unit that tracks if waitUntilDurable() is called.
 */
class RecoveryUnitWithDurabilityTracking : public RecoveryUnitNoop {
public:
    bool waitUntilDurable() override;
    bool waitUntilDurableCalled = false;
};

bool RecoveryUnitWithDurabilityTracking::waitUntilDurable() {
    waitUntilDurableCalled = true;
    return RecoveryUnitNoop::waitUntilDurable();
}

TEST_F(ReplicationConsistencyMarkersTest, InitialSyncFlag) {
    auto nss = makeNamespace(_agent);

    ReplicationConsistencyMarkersImpl minValid(getStorageInterface(), nss);
    auto opCtx = getOperationContext();
    minValid.initializeMinValidDocument(opCtx);

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(minValid.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    minValid.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(minValid.getInitialSyncFlag(opCtx));

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, nss);
    ASSERT_TRUE(minValidDocument.hasField(MinValidDocument::kInitialSyncFlagFieldName));
    ASSERT_TRUE(minValidDocument.getBoolField(MinValidDocument::kInitialSyncFlagFieldName));

    // Clearing initial sync flag should affect getInitialSyncFlag() result.
    minValid.clearInitialSyncFlag(opCtx);
    ASSERT_FALSE(minValid.getInitialSyncFlag(opCtx));
}

TEST_F(ReplicationConsistencyMarkersTest, GetMinValidAfterSettingInitialSyncFlagWorks) {
    auto nss = makeNamespace(_agent);

    ReplicationConsistencyMarkersImpl minValid(getStorageInterface(), nss);
    auto opCtx = getOperationContext();
    minValid.initializeMinValidDocument(opCtx);

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(minValid.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    minValid.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(minValid.getInitialSyncFlag(opCtx));

    ASSERT(minValid.getMinValid(opCtx).isNull());
    ASSERT(minValid.getAppliedThrough(opCtx).isNull());
    ASSERT(minValid.getOplogDeleteFromPoint(opCtx).isNull());
}

TEST_F(ReplicationConsistencyMarkersTest, ReplicationConsistencyMarkers) {
    auto nss = makeNamespace(_agent);

    ReplicationConsistencyMarkersImpl minValid(getStorageInterface(), nss);
    auto opCtx = getOperationContext();
    minValid.initializeMinValidDocument(opCtx);

    // MinValid boundaries should all be null after initializing a new storage engine.
    ASSERT(minValid.getMinValid(opCtx).isNull());
    ASSERT(minValid.getAppliedThrough(opCtx).isNull());
    ASSERT(minValid.getOplogDeleteFromPoint(opCtx).isNull());

    // Setting min valid boundaries should affect getMinValid() result.
    OpTime startOpTime({Seconds(123), 0}, 1LL);
    OpTime endOpTime({Seconds(456), 0}, 1LL);
    minValid.setAppliedThrough(opCtx, startOpTime);
    minValid.setMinValid(opCtx, endOpTime);
    minValid.setOplogDeleteFromPoint(opCtx, endOpTime.getTimestamp());

    ASSERT_EQ(minValid.getAppliedThrough(opCtx), startOpTime);
    ASSERT_EQ(minValid.getMinValid(opCtx), endOpTime);
    ASSERT_EQ(minValid.getOplogDeleteFromPoint(opCtx), endOpTime.getTimestamp());

    // setMinValid always changes minValid, but setMinValidToAtLeast only does if higher.
    minValid.setMinValid(opCtx, startOpTime);  // Forcibly lower it.
    ASSERT_EQ(minValid.getMinValid(opCtx), startOpTime);
    minValid.setMinValidToAtLeast(opCtx, endOpTime);  // Higher than current (sets it).
    ASSERT_EQ(minValid.getMinValid(opCtx), endOpTime);
    minValid.setMinValidToAtLeast(opCtx, startOpTime);  // Lower than current (no-op).
    ASSERT_EQ(minValid.getMinValid(opCtx), endOpTime);

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, nss);
    ASSERT_TRUE(minValidDocument.hasField(MinValidDocument::kAppliedThroughFieldName));
    ASSERT_TRUE(minValidDocument[MinValidDocument::kAppliedThroughFieldName].isABSONObj());
    ASSERT_EQUALS(startOpTime,
                  unittest::assertGet(OpTime::parseFromOplogEntry(
                      minValidDocument[MinValidDocument::kAppliedThroughFieldName].Obj())));
    ASSERT_EQUALS(endOpTime, unittest::assertGet(OpTime::parseFromOplogEntry(minValidDocument)));
    ASSERT_EQUALS(endOpTime.getTimestamp(),
                  minValidDocument[MinValidDocument::kOplogDeleteFromPointFieldName].timestamp());

    // Recovery unit will be owned by "opCtx".
    RecoveryUnitWithDurabilityTracking* recoveryUnit = new RecoveryUnitWithDurabilityTracking();
    opCtx->setRecoveryUnit(recoveryUnit, OperationContext::kNotInUnitOfWork);

    // Set min valid without waiting for the changes to be durable.
    OpTime endOpTime2({Seconds(789), 0}, 1LL);
    minValid.setMinValid(opCtx, endOpTime2);
    minValid.setAppliedThrough(opCtx, {});
    ASSERT_EQUALS(minValid.getAppliedThrough(opCtx), OpTime());
    ASSERT_EQUALS(minValid.getMinValid(opCtx), endOpTime2);
    ASSERT_FALSE(recoveryUnit->waitUntilDurableCalled);
}

}  // namespace

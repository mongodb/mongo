/**
 *    Copyright (C) 2016 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/db/service_context_d_test_fixture.h"

#include "bongo/base/checked_cast.h"
#include "bongo/db/catalog/database.h"
#include "bongo/db/catalog/database_holder.h"
#include "bongo/db/client.h"
#include "bongo/db/concurrency/write_conflict_exception.h"
#include "bongo/db/curop.h"
#include "bongo/db/db_raii.h"
#include "bongo/db/logical_clock.h"
#include "bongo/db/op_observer_noop.h"
#include "bongo/db/operation_context.h"
#include "bongo/db/service_context.h"
#include "bongo/db/service_context_d.h"
#include "bongo/db/storage/storage_options.h"
#include "bongo/db/time_proof_service.h"
#include "bongo/stdx/memory.h"
#include "bongo/unittest/temp_dir.h"
#include "bongo/util/scopeguard.h"

namespace bongo {

void ServiceContextBongoDTest::setUp() {
    Client::initThread(getThreadName().c_str());
    ServiceContext* serviceContext = getServiceContext();

    std::array<std::uint8_t, 20> tempKey = {};
    TimeProofService::Key key(std::move(tempKey));
    auto timeProofService = stdx::make_unique<TimeProofService>(std::move(key));
    auto logicalClock =
        stdx::make_unique<LogicalClock>(serviceContext, std::move(timeProofService), false);
    LogicalClock::set(serviceContext, std::move(logicalClock));

    if (!serviceContext->getGlobalStorageEngine()) {
        // When using the "ephemeralForTest" storage engine, it is fine for the temporary directory
        // to go away after the global storage engine is initialized.
        unittest::TempDir tempDir("service_context_d_test_fixture");
        storageGlobalParams.dbpath = tempDir.path();
        storageGlobalParams.engine = "ephemeralForTest";
        storageGlobalParams.engineSetByUser = true;
        checked_cast<ServiceContextBongoD*>(getGlobalServiceContext())->createLockFile();
        serviceContext->initializeGlobalStorageEngine();
        serviceContext->setOpObserver(stdx::make_unique<OpObserverNoop>());
    }
}

void ServiceContextBongoDTest::tearDown() {
    ON_BLOCK_EXIT([&] { Client::destroy(); });
    auto txn = cc().makeOperationContext();
    _dropAllDBs(txn.get());
}

ServiceContext* ServiceContextBongoDTest::getServiceContext() {
    return getGlobalServiceContext();
}

void ServiceContextBongoDTest::_dropAllDBs(OperationContext* txn) {
    dropAllDatabasesExceptLocal(txn);

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());
    AutoGetDb autoDBLocal(txn, "local", MODE_X);
    const auto localDB = autoDBLocal.getDb();
    if (localDB) {
        BONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            // Do not wrap in a WriteUnitOfWork until SERVER-17103 is addressed.
            autoDBLocal.getDb()->dropDatabase(txn, localDB);
        }
        BONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "_dropAllDBs", "local");
    }

    // dropAllDatabasesExceptLocal() does not close empty databases. However the holder still
    // allocates resources to track these empty databases. These resources not released by
    // dropAllDatabasesExceptLocal() will be leaked at exit unless we call DatabaseHolder::closeAll.
    BSONObjBuilder unused;
    invariant(dbHolder().closeAll(txn, unused, false));
}

}  // namespace bongo

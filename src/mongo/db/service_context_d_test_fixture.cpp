/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_d_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void ServiceContextMongoDTest::setUp() {
    Client::initThread(getThreadName());

    auto const serviceContext = getServiceContext();

    auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext);
    LogicalClock::set(serviceContext, std::move(logicalClock));

    if (!serviceContext->getGlobalStorageEngine()) {
        // When using the "ephemeralForTest" storage engine, it is fine for the temporary directory
        // to go away after the global storage engine is initialized.
        unittest::TempDir tempDir("service_context_d_test_fixture");
        storageGlobalParams.dbpath = tempDir.path();
        storageGlobalParams.engine = "ephemeralForTest";
        storageGlobalParams.engineSetByUser = true;

        checked_cast<ServiceContextMongoD*>(serviceContext)->createLockFile();
        serviceContext->initializeGlobalStorageEngine();
        serviceContext->setOpObserver(stdx::make_unique<OpObserverNoop>());
    }
}

void ServiceContextMongoDTest::tearDown() {
    ON_BLOCK_EXIT([&] { Client::destroy(); });
    auto opCtx = cc().makeOperationContext();
    _dropAllDBs(opCtx.get());
}

ServiceContext* ServiceContextMongoDTest::getServiceContext() {
    return getGlobalServiceContext();
}

void ServiceContextMongoDTest::_doTest() {
    MONGO_UNREACHABLE;
}

void ServiceContextMongoDTest::_dropAllDBs(OperationContext* opCtx) {
    dropAllDatabasesExceptLocal(opCtx);

    Lock::GlobalWrite lk(opCtx);
    AutoGetDb autoDBLocal(opCtx, "local", MODE_X);
    const auto localDB = autoDBLocal.getDb();
    if (localDB) {
        writeConflictRetry(opCtx, "_dropAllDBs", "local", [&] {
            // Do not wrap in a WriteUnitOfWork until SERVER-17103 is addressed.
            autoDBLocal.getDb()->dropDatabase(opCtx, localDB);
        });
    }

    // dropAllDatabasesExceptLocal() does not close empty databases. However the holder still
    // allocates resources to track these empty databases. These resources not released by
    // dropAllDatabasesExceptLocal() will be leaked at exit unless we call DatabaseHolder::closeAll.
    BSONObjBuilder unused;
    invariant(dbHolder().closeAll(opCtx, unused, false, "all databases dropped"));
}

}  // namespace mongo

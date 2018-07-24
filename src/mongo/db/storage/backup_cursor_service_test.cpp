/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/storage/backup_cursor_service.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class BackupCursorServiceTest : public ServiceContextMongoDTest {
public:
    BackupCursorServiceTest()
        : ServiceContextMongoDTest("devnull"),
          _opCtx(cc().makeOperationContext()),
          _storageEngine(getServiceContext()->getStorageEngine()),
          _backupCursorService(stdx::make_unique<BackupCursorService>(_storageEngine)) {}

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    StorageEngine* _storageEngine;
    std::unique_ptr<BackupCursorService> _backupCursorService;
};

TEST_F(BackupCursorServiceTest, TestTypicalLifetime) {
    _backupCursorService->fsyncLock(_opCtx.get());
    _backupCursorService->fsyncUnlock(_opCtx.get());
}

TEST_F(BackupCursorServiceTest, TestDoubleLock) {
    _backupCursorService->fsyncLock(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncLock(_opCtx.get()),
                       DBException,
                       "The existing backup cursor must be closed before fsyncLock can succeed.");
    _backupCursorService->fsyncUnlock(_opCtx.get());
}

TEST_F(BackupCursorServiceTest, TestDoubleUnlock) {
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncUnlock(_opCtx.get()),
                       DBException,
                       "There is no backup cursor to close with fsyncUnlock.");

    _backupCursorService->fsyncLock(_opCtx.get());
    _backupCursorService->fsyncUnlock(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncUnlock(_opCtx.get()),
                       DBException,
                       "There is no backup cursor to close with fsyncUnlock.");
}


}  // namespace
}  // namespace mongo

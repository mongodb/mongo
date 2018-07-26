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

TEST_F(BackupCursorServiceTest, TestTypicalFsyncLifetime) {
    _backupCursorService->fsyncLock(_opCtx.get());
    _backupCursorService->fsyncUnlock(_opCtx.get());

    _backupCursorService->fsyncLock(_opCtx.get());
    _backupCursorService->fsyncUnlock(_opCtx.get());
}

TEST_F(BackupCursorServiceTest, TestDoubleLock) {
    _backupCursorService->fsyncLock(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncLock(_opCtx.get()),
                       DBException,
                       "The node is already fsyncLocked.");
    _backupCursorService->fsyncUnlock(_opCtx.get());
}

TEST_F(BackupCursorServiceTest, TestDoubleUnlock) {
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncUnlock(_opCtx.get()),
                       DBException,
                       "The node is not fsyncLocked.");

    _backupCursorService->fsyncLock(_opCtx.get());
    _backupCursorService->fsyncUnlock(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncUnlock(_opCtx.get()),
                       DBException,
                       "The node is not fsyncLocked.");
}

TEST_F(BackupCursorServiceTest, TestTypicalCursorLifetime) {
    auto backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());
    ASSERT_EQUALS(1u, backupCursorState.cursorId);
    ASSERT_EQUALS(1u, backupCursorState.filenames.size());
    ASSERT_EQUALS("filename.wt", backupCursorState.filenames[0]);

    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);

    backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());
    ASSERT_EQUALS(2u, backupCursorState.cursorId);
    ASSERT_EQUALS(1u, backupCursorState.filenames.size());
    ASSERT_EQUALS("filename.wt", backupCursorState.filenames[0]);

    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);
}

TEST_F(BackupCursorServiceTest, TestDoubleOpenCursor) {
    auto backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());
    ASSERT_THROWS_WHAT(
        _backupCursorService->openBackupCursor(_opCtx.get()),
        DBException,
        "The existing backup cursor must be closed before $backupCursor can succeed.");
    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);
}

TEST_F(BackupCursorServiceTest, TestDoubleCloseCursor) {
    ASSERT_THROWS_WHAT(_backupCursorService->closeBackupCursor(_opCtx.get(), 10),
                       DBException,
                       "There is no backup cursor to close.");

    auto backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());
    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);
    ASSERT_THROWS_WHAT(
        _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId),
        DBException,
        "There is no backup cursor to close.");
}

TEST_F(BackupCursorServiceTest, TestCloseWrongCursor) {
    auto backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());

    ASSERT_THROWS_WITH_CHECK(
        _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId + 1),
        DBException,
        [](const DBException& exc) {
            ASSERT_STRING_CONTAINS(exc.what(), "Can only close the running backup cursor.");
        });

    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);
}

TEST_F(BackupCursorServiceTest, TestMixingFsyncAndCursors) {
    _backupCursorService->fsyncLock(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->openBackupCursor(_opCtx.get()),
                       DBException,
                       "The node is currently fsyncLocked.");
    ASSERT_THROWS_WHAT(_backupCursorService->closeBackupCursor(_opCtx.get(), 1),
                       DBException,
                       "There is no backup cursor to close.");
    _backupCursorService->fsyncUnlock(_opCtx.get());

    auto backupCursorState = _backupCursorService->openBackupCursor(_opCtx.get());
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncLock(_opCtx.get()),
                       DBException,
                       "The existing backup cursor must be closed before fsyncLock can succeed.");
    ASSERT_THROWS_WHAT(_backupCursorService->fsyncUnlock(_opCtx.get()),
                       DBException,
                       "The node is not fsyncLocked.");
    _backupCursorService->closeBackupCursor(_opCtx.get(), backupCursorState.cursorId);
}

}  // namespace
}  // namespace mongo

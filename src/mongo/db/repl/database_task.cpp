/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/database_task.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

// static
DatabaseTask::Task DatabaseTask::makeGlobalExclusiveLockTask(const Task& task) {
    invariant(task);
    DatabaseTask::Task newTask = [task](OperationContext* txn, const Status& status) {
        if (!status.isOK()) {
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite lock(txn->lockState());
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "globalExclusiveLockTask", "global");
        MONGO_UNREACHABLE;
    };
    return newTask;
}

// static
DatabaseTask::Task DatabaseTask::makeDatabaseLockTask(const Task& task,
                                                      const std::string& databaseName,
                                                      LockMode mode) {
    invariant(task);
    DatabaseTask::Task newTask = [=](OperationContext* txn, const Status& status) {
        if (!status.isOK()) {
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            LockMode permissiveLockMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
            ScopedTransaction transaction(txn, permissiveLockMode);
            Lock::DBLock lock(txn->lockState(), databaseName, mode);
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "databaseLockTask", databaseName);
        MONGO_UNREACHABLE;
    };
    return newTask;
}

// static
DatabaseTask::Task DatabaseTask::makeCollectionLockTask(const Task& task,
                                                        const NamespaceString& nss,
                                                        LockMode mode) {
    invariant(task);
    DatabaseTask::Task newTask = [=](OperationContext* txn, const Status& status) {
        if (!status.isOK()) {
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            LockMode permissiveLockMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
            ScopedTransaction transaction(txn, permissiveLockMode);
            Lock::DBLock lock(txn->lockState(), nss.db(), permissiveLockMode);
            Lock::CollectionLock collectionLock(txn->lockState(), nss.toString(), mode);
            return task(txn, status);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "collectionLockTask", nss.toString());
        MONGO_UNREACHABLE;
    };
    return newTask;
}

}  // namespace repl
}  // namespace mongo

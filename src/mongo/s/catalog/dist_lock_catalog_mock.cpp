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

#include "mongo/s/catalog/dist_lock_catalog_mock.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    Status kBadRetValue(ErrorCodes::InternalError, "no return value");
    StatusWith<LocksType> kLocksTypeBadRetValue(kBadRetValue);

    void noGrabLockFuncSet(StringData lockID,
                           const OID& lockSessionID,
                           StringData who,
                           StringData processId,
                           Date_t time,
                           StringData why) {
        FAIL(str::stream() << "grabLock not expected to be called. "
                           << "lockID: " << lockID
                           << ", who: " << who
                           << ", processId: " << processId
                           << ", why: " << why);
    };

    void noUnLockFuncSet(const OID& lockSessionID) {
        FAIL(str::stream() << "unlock not expected to be called. "
                           << "lockSessionID: " << lockSessionID);
    };
} // unnamed namespace

    DistLockCatalogMock::DistLockCatalogMock():
            _grabLockChecker(noGrabLockFuncSet),
            _grabLockReturnValue(kLocksTypeBadRetValue),
            _unlockChecker(noUnLockFuncSet),
            _unlockReturnValue(kBadRetValue) {
    }

    DistLockCatalogMock::~DistLockCatalogMock() {
    }

    StatusWith<LockpingsType> DistLockCatalogMock::getPing(StringData processID) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    Status DistLockCatalogMock::ping(StringData processID, Date_t ping) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    StatusWith<LocksType> DistLockCatalogMock::grabLock(StringData lockID,
                                                        const OID& lockSessionID,
                                                        StringData who,
                                                        StringData processId,
                                                        Date_t time,
                                                        StringData why) {
        auto ret = kLocksTypeBadRetValue;
        GrabLockFunc checkerFunc = noGrabLockFuncSet;

        {
            stdx::lock_guard<stdx::mutex> ul(_mutex);
            ret = _grabLockReturnValue;
            checkerFunc = _grabLockChecker;
        }

        checkerFunc(lockID, lockSessionID, who, processId, time, why);
        return ret;
    }

    StatusWith<LocksType> DistLockCatalogMock::overtakeLock(StringData lockID,
                                                            const OID& lockSessionID,
                                                            const OID& lockTS,
                                                            StringData who,
                                                            StringData processId,
                                                            Date_t time,
                                                            StringData why) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    Status DistLockCatalogMock::unlock(const OID& lockSessionID) {
        auto ret = kBadRetValue;
        UnlockFunc checkerFunc = noUnLockFuncSet;

        {
            stdx::lock_guard<stdx::mutex> ul(_mutex);
            ret = _unlockReturnValue;
            checkerFunc = _unlockChecker;
        }

        checkerFunc(lockSessionID);
        return ret;
    }

    StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogMock::getServerInfo() {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    StatusWith<LocksType> DistLockCatalogMock::getLockByTS(const OID& ts) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    void DistLockCatalogMock::setSucceedingExpectedGrabLock(
            DistLockCatalogMock::GrabLockFunc checkerFunc,
            StatusWith<LocksType> returnThis) {
        stdx::lock_guard<stdx::mutex> ul(_mutex);
        _grabLockChecker = checkerFunc;
        _grabLockReturnValue = returnThis;
    }

    void DistLockCatalogMock::expectNoGrabLock() {
        stdx::lock_guard<stdx::mutex> ul(_mutex);
        _grabLockChecker = noGrabLockFuncSet;
        _grabLockReturnValue = kLocksTypeBadRetValue;
    }

    void DistLockCatalogMock::setSucceedingExpectedUnLock(
            DistLockCatalogMock::UnlockFunc checkerFunc,
            Status returnThis) {
        stdx::lock_guard<stdx::mutex> ul(_mutex);
        _unlockChecker = checkerFunc;
        _unlockReturnValue = returnThis;
    }

}

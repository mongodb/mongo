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
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
Status kBadRetValue(ErrorCodes::InternalError, "no return value");
StatusWith<LocksType> kLocksTypeBadRetValue(kBadRetValue);
StatusWith<LockpingsType> kLockpingsTypeBadRetValue(kBadRetValue);
StatusWith<DistLockCatalog::ServerInfo> kServerInfoBadRetValue(kBadRetValue);

void noGrabLockFuncSet(StringData lockID,
                       const OID& lockSessionID,
                       StringData who,
                       StringData processId,
                       Date_t time,
                       StringData why) {
    FAIL(str::stream() << "grabLock not expected to be called. "
                       << "lockID: "
                       << lockID
                       << ", who: "
                       << who
                       << ", processId: "
                       << processId
                       << ", why: "
                       << why);
}

void noOvertakeLockFuncSet(StringData lockID,
                           const OID& lockSessionID,
                           const OID& currentHolderTS,
                           StringData who,
                           StringData processId,
                           Date_t time,
                           StringData why) {
    FAIL(str::stream() << "overtakeLock not expected to be called. "
                       << "lockID: "
                       << lockID
                       << ", currentHolderTS: "
                       << currentHolderTS
                       << ", who: "
                       << who
                       << ", processId: "
                       << processId
                       << ", why: "
                       << why);
}

void noUnLockFuncSet(const OID& lockSessionID) {
    FAIL(str::stream() << "unlock not expected to be called. "
                       << "lockSessionID: "
                       << lockSessionID);
}

void noPingFuncSet(StringData processID, Date_t ping) {
    // Ping is expected to be called all the time, so default behavior is do nothing.
}

void noStopPingFuncSet(StringData processID) {
    FAIL(str::stream() << "stopPing not expected to be called. "
                       << "processID: "
                       << processID);
}

void noGetLockByTSSet(const OID& lockSessionID) {
    FAIL(str::stream() << "getLockByTS not expected to be called. "
                       << "lockSessionID: "
                       << lockSessionID);
}

void noGetLockByNameSet(StringData name) {
    FAIL(str::stream() << "getLockByName not expected to be called. "
                       << "lockName: "
                       << name);
}

void noGetPingSet(StringData processId) {
    FAIL(str::stream() << "getPing not expected to be called. "
                       << "lockName: "
                       << processId);
}

void noGetServerInfoSet() {
    FAIL("getServerInfo not expected to be called");
}

}  // unnamed namespace

DistLockCatalogMock::DistLockCatalogMock()
    : _grabLockChecker(noGrabLockFuncSet),
      _grabLockReturnValue(kLocksTypeBadRetValue),
      _unlockChecker(noUnLockFuncSet),
      _unlockReturnValue(kBadRetValue),
      _pingChecker(noPingFuncSet),
      _pingReturnValue(kBadRetValue),
      _stopPingChecker(noStopPingFuncSet),
      _stopPingReturnValue(kBadRetValue),
      _getLockByTSChecker(noGetLockByTSSet),
      _getLockByTSReturnValue(kLocksTypeBadRetValue),
      _getLockByNameChecker(noGetLockByNameSet),
      _getLockByNameReturnValue(kLocksTypeBadRetValue),
      _overtakeLockChecker(noOvertakeLockFuncSet),
      _overtakeLockReturnValue(kLocksTypeBadRetValue),
      _getPingChecker(noGetPingSet),
      _getPingReturnValue(kLockpingsTypeBadRetValue),
      _getServerInfoChecker(noGetServerInfoSet),
      _getServerInfoReturnValue(kServerInfoBadRetValue) {}

DistLockCatalogMock::~DistLockCatalogMock() {}

StatusWith<LockpingsType> DistLockCatalogMock::getPing(OperationContext* txn,
                                                       StringData processID) {
    auto ret = kLockpingsTypeBadRetValue;
    GetPingFunc checkerFunc = noGetPingSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _getPingReturnValue;
        checkerFunc = _getPingChecker;
    }

    checkerFunc(processID);
    return ret;
}

Status DistLockCatalogMock::ping(OperationContext* txn, StringData processID, Date_t ping) {
    auto ret = kBadRetValue;
    PingFunc checkerFunc = noPingFuncSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _pingReturnValue;
        checkerFunc = _pingChecker;
    }

    checkerFunc(processID, ping);
    return ret;
}

StatusWith<LocksType> DistLockCatalogMock::grabLock(OperationContext* txn,
                                                    StringData lockID,
                                                    const OID& lockSessionID,
                                                    StringData who,
                                                    StringData processId,
                                                    Date_t time,
                                                    StringData why) {
    auto ret = kLocksTypeBadRetValue;
    GrabLockFunc checkerFunc = noGrabLockFuncSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _grabLockReturnValue;
        checkerFunc = _grabLockChecker;
    }

    checkerFunc(lockID, lockSessionID, who, processId, time, why);
    return ret;
}

StatusWith<LocksType> DistLockCatalogMock::overtakeLock(OperationContext* txn,
                                                        StringData lockID,
                                                        const OID& lockSessionID,
                                                        const OID& currentHolderTS,
                                                        StringData who,
                                                        StringData processId,
                                                        Date_t time,
                                                        StringData why) {
    auto ret = kLocksTypeBadRetValue;
    OvertakeLockFunc checkerFunc = noOvertakeLockFuncSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _overtakeLockReturnValue;
        checkerFunc = _overtakeLockChecker;
    }

    checkerFunc(lockID, lockSessionID, currentHolderTS, who, processId, time, why);
    return ret;
}

Status DistLockCatalogMock::unlock(OperationContext* txn, const OID& lockSessionID) {
    auto ret = kBadRetValue;
    UnlockFunc checkerFunc = noUnLockFuncSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _unlockReturnValue;
        checkerFunc = _unlockChecker;
    }

    checkerFunc(lockSessionID);
    return ret;
}

StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogMock::getServerInfo(OperationContext* txn) {
    auto ret = kServerInfoBadRetValue;
    GetServerInfoFunc checkerFunc = noGetServerInfoSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _getServerInfoReturnValue;
        checkerFunc = _getServerInfoChecker;
    }

    checkerFunc();
    return ret;
}

StatusWith<LocksType> DistLockCatalogMock::getLockByTS(OperationContext* txn,
                                                       const OID& lockSessionID) {
    auto ret = kLocksTypeBadRetValue;
    GetLockByTSFunc checkerFunc = noGetLockByTSSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _getLockByTSReturnValue;
        checkerFunc = _getLockByTSChecker;
    }

    checkerFunc(lockSessionID);
    return ret;
}

StatusWith<LocksType> DistLockCatalogMock::getLockByName(OperationContext* txn, StringData name) {
    auto ret = kLocksTypeBadRetValue;
    GetLockByNameFunc checkerFunc = noGetLockByNameSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _getLockByNameReturnValue;
        checkerFunc = _getLockByNameChecker;
    }

    checkerFunc(name);
    return ret;
}

Status DistLockCatalogMock::stopPing(OperationContext* txn, StringData processId) {
    auto ret = kBadRetValue;
    StopPingFunc checkerFunc = noStopPingFuncSet;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ret = _stopPingReturnValue;
        checkerFunc = _stopPingChecker;
    }

    checkerFunc(processId);
    return ret;
}

void DistLockCatalogMock::expectGrabLock(DistLockCatalogMock::GrabLockFunc checkerFunc,
                                         StatusWith<LocksType> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _grabLockChecker = checkerFunc;
    _grabLockReturnValue = returnThis;
}

void DistLockCatalogMock::expectNoGrabLock() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _grabLockChecker = noGrabLockFuncSet;
    _grabLockReturnValue = kLocksTypeBadRetValue;
}

void DistLockCatalogMock::expectUnLock(DistLockCatalogMock::UnlockFunc checkerFunc,
                                       Status returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _unlockChecker = checkerFunc;
    _unlockReturnValue = returnThis;
}

void DistLockCatalogMock::expectPing(DistLockCatalogMock::PingFunc checkerFunc, Status returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _pingChecker = checkerFunc;
    _pingReturnValue = returnThis;
}

void DistLockCatalogMock::expectStopPing(StopPingFunc checkerFunc, Status returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopPingChecker = checkerFunc;
    _stopPingReturnValue = returnThis;
}

void DistLockCatalogMock::expectGetLockByTS(GetLockByTSFunc checkerFunc,
                                            StatusWith<LocksType> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _getLockByTSChecker = checkerFunc;
    _getLockByTSReturnValue = returnThis;
}

void DistLockCatalogMock::expectGetLockByName(GetLockByNameFunc checkerFunc,
                                              StatusWith<LocksType> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _getLockByNameChecker = checkerFunc;
    _getLockByNameReturnValue = returnThis;
}

void DistLockCatalogMock::expectOvertakeLock(OvertakeLockFunc checkerFunc,
                                             StatusWith<LocksType> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _overtakeLockChecker = checkerFunc;
    _overtakeLockReturnValue = returnThis;
}

void DistLockCatalogMock::expectGetPing(GetPingFunc checkerFunc,
                                        StatusWith<LockpingsType> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _getPingChecker = checkerFunc;
    _getPingReturnValue = returnThis;
}

void DistLockCatalogMock::expectGetServerInfo(GetServerInfoFunc checkerFunc,
                                              StatusWith<DistLockCatalog::ServerInfo> returnThis) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _getServerInfoChecker = checkerFunc;
    _getServerInfoReturnValue = returnThis;
}

Status DistLockCatalogMock::unlockAll(OperationContext* txn, const std::string& processID) {
    return Status(ErrorCodes::IllegalOperation,
                  str::stream() << "unlockAll not expected to be called; processID: " << processID);
}
}

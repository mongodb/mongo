/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/mutex.h"

namespace mongo {

namespace {
std::unique_ptr<LockActions> gLockActions;
}

void Mutex::lock() {
    auto hasLock = _mutex.try_lock_for(kContendedLockTimeout.toSystemDuration());
    if (hasLock) {
        return;
    }
    if (gLockActions) {
        gLockActions->onContendedLock(_name);
    }
    hasLock = _mutex.try_lock_for(_lockTimeout.toSystemDuration() -
                                  kContendedLockTimeout.toSystemDuration());
    uassert(
        ErrorCodes::InternalError, "Unable to take latch, wait time exceeds set timeout", hasLock);
}
void Mutex::unlock() {
    if (gLockActions) {
        gLockActions->onUnlock();
    }
    _mutex.unlock();
}
bool Mutex::try_lock() {
    return _mutex.try_lock();
}

void Mutex::setLockActions(std::unique_ptr<LockActions> actions) {
    gLockActions = std::move(actions);
}

}  // namespace mongo

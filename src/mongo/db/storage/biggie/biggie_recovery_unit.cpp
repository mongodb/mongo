// biggie_recovery_unit.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/biggie/biggie_recovery_unit.h"

#include <mutex>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/util/log.h"

namespace mongo {
namespace biggie {

RecoveryUnit::RecoveryUnit(KVEngine* parentKVEngine, stdx::function<void()> cb)
    : _waitUntilDurableCallback(cb), _KVEngine(parentKVEngine) {}

void RecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {}

void RecoveryUnit::commitUnitOfWork() {
    while (true) {
        std::shared_ptr<StringStore> master = _KVEngine->getMaster();
        try {
            _workingCopy->merge3(*_mergeBase, *master);
        } catch (const merge_conflict_exception&) {
            throw WriteConflictException();
        }
        stdx::lock_guard<stdx::mutex> lkOnMaster(_KVEngine->getMasterLock());
        if (_KVEngine->getMaster_inlock() == master) {
            _KVEngine->setMaster_inlock(std::move(_workingCopy));
            _mergeBase.reset();
            break;
        }
    }
    try {
        for (Changes::iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit(boost::none);
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }
}

void RecoveryUnit::abortUnitOfWork() {
    _workingCopy.reset();
    _mergeBase.reset();
    try {
        for (Changes::reverse_iterator it = _changes.rbegin(), end = _changes.rend(); it != end;
             ++it) {
            ChangePtr change = *it;
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*change));
            change->rollback();
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }
}

bool RecoveryUnit::waitUntilDurable() {
    return true;  // This is an in-memory storage engine.
}

void RecoveryUnit::abandonSnapshot() {
    _mergeBase.reset();
    _workingCopy.reset();
    // TODO : check if we need to add something later.
}

void RecoveryUnit::registerChange(Change* change) {
    _changes.push_back(ChangePtr(change));
}

SnapshotId RecoveryUnit::getSnapshotId() const {
    return SnapshotId();
}

bool RecoveryUnit::forkIfNeeded() {
    // _workingCopy and _mergeBase either both exist or both don't.
    invariant((_workingCopy && _mergeBase) || (!_workingCopy && !_mergeBase));
    if (_mergeBase) {
        return false;
    }
    // TODO : later on this needs to be changed to use their copy function.
    _mergeBase = _KVEngine->getMaster();
    // TODO : later on this needs to be changed to use their copy function.
    _workingCopy = std::make_unique<StringStore>(*_mergeBase);
    return true;
}

void RecoveryUnit::setOrderedCommit(bool orderedCommit) {}

}  // namespace biggie
}  // namespace mongo

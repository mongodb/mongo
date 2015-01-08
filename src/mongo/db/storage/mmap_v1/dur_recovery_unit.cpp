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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"

#include <algorithm>
#include <string>

#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    DurRecoveryUnit::DurRecoveryUnit() : _mustRollback(false) {

    }

    void DurRecoveryUnit::beginUnitOfWork() {
        _startOfUncommittedChangesForLevel.push_back(Indexes(_changes.size(), _writes.size()));
    }

    void DurRecoveryUnit::commitUnitOfWork() {
        invariant(inAUnitOfWork());
        invariant(!_mustRollback);

        if (!inOutermostUnitOfWork()) {
            // If we are nested, make all changes for this level part of the containing UnitOfWork.
            // They will be added to the global damages list once the outermost UnitOfWork commits,
            // which it must now do.
            if (haveUncommitedChangesAtCurrentLevel()) {
                _startOfUncommittedChangesForLevel.back() =
                    Indexes(_changes.size(), _writes.size());
            }
            return;
        }

        commitChanges();

        // global journal flush opportunity
        getDur().commitIfNeeded();
    }

    void DurRecoveryUnit::endUnitOfWork() {
        invariant(inAUnitOfWork());

        if (haveUncommitedChangesAtCurrentLevel()) {
            rollbackInnermostChanges();
        }

        _startOfUncommittedChangesForLevel.pop_back();
    }

    void DurRecoveryUnit::commitAndRestart() {
        invariant( !inAUnitOfWork() );
        // no-op since we have no transaction
    }

    void DurRecoveryUnit::commitChanges() {
        if (!inAUnitOfWork())
            return;

        invariant(!_mustRollback);
        invariant(inOutermostUnitOfWork());
        invariant(_startOfUncommittedChangesForLevel.front().changeIndex == 0);
        invariant(_startOfUncommittedChangesForLevel.front().writeIndex == 0);

        if (getDur().isDurable())
            pushChangesToDurSubSystem();

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }

        // We now reset to a "clean" state without any uncommited changes.
        _changes.clear();
        _writes.clear();
        _preimageBuffer.clear();
    }

    void DurRecoveryUnit::pushChangesToDurSubSystem() {
        if (_writes.empty())
            return;

        typedef std::pair<void*, unsigned> Intent;
        std::vector<Intent> intents;
        intents.reserve(_writes.size());

        // orders by addr so we can coalesce overlapping and adjacent writes
        std::sort(_writes.begin(), _writes.end());

        intents.push_back(std::make_pair(_writes.front().addr, _writes.front().len));
        for (Writes::iterator it = (_writes.begin() + 1), end = _writes.end(); it != end; ++it) {
            Intent& lastIntent = intents.back();
            char* lastEnd = static_cast<char*>(lastIntent.first) + lastIntent.second;
            if (it->addr <= lastEnd) {
                // overlapping or adjacent, so extend.
                ptrdiff_t extendedLen = (it->addr + it->len) - static_cast<char*>(lastIntent.first);
                lastIntent.second = std::max(lastIntent.second, unsigned(extendedLen));
            }
            else {
                // not overlapping, so create a new intent
                intents.push_back(std::make_pair(it->addr, it->len));
            }
        }

        getDur().declareWriteIntents(intents);
    }

    void DurRecoveryUnit::rollbackInnermostChanges() {
        // Using signed ints to avoid issues in loops below around index 0.
        invariant(_changes.size() <= size_t(std::numeric_limits<int>::max()));
        invariant(_writes.size() <= size_t(std::numeric_limits<int>::max()));
        const int changesRollbackTo = _startOfUncommittedChangesForLevel.back().changeIndex;
        const int writesRollbackTo = _startOfUncommittedChangesForLevel.back().writeIndex;

        LOG(2) << "   ***** ROLLING BACK " << (_writes.size() - writesRollbackTo) << " disk writes"
               << " and " << (_changes.size() - changesRollbackTo) << " custom changes";

        // First rollback disk writes, then Changes. This matches behavior in other storage engines
        // that either rollback a transaction or don't write a writebatch.

        for (int i = _writes.size() - 1; i >= writesRollbackTo; i--) {
            // TODO need to add these pages to our "dirty count" somehow.
            _preimageBuffer.copy(_writes[i].addr, _writes[i].len, _writes[i].offset);
        }

        for (int i = _changes.size() - 1; i >= changesRollbackTo; i--) {
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*_changes[i]));
            _changes[i]->rollback();
        }

        _writes.erase(_writes.begin() + writesRollbackTo, _writes.end());
        _changes.erase(_changes.begin() + changesRollbackTo, _changes.end());

        if (inOutermostUnitOfWork()) {
            // We just rolled back so we are now "clean" and don't need to roll back anymore.
            invariant(_changes.empty());
            invariant(_writes.empty());
            _preimageBuffer.clear();
            _mustRollback = false;
        }
        else {
            // Inner UOW rolled back, so outer must not commit. We can loosen this in the future,
            // but that would require all StorageEngines to support rollback of nested transactions.
            _mustRollback = true;
        }
    }

    bool DurRecoveryUnit::awaitCommit() {
        invariant(!inAUnitOfWork());
        return getDur().awaitCommit();
    }

    void* DurRecoveryUnit::writingPtr(void* data, size_t len) {
        invariant(inAUnitOfWork());

        if (len == 0) return data; // Don't need to do anything for empty ranges.
        invariant(len < size_t(std::numeric_limits<int>::max()));

        // Windows requires us to adjust the address space *before* we write to anything.
        privateViews.makeWritable(data, len);

        _writes.push_back(Write(static_cast<char*>(data), len, _preimageBuffer.size()));
        _preimageBuffer.append(static_cast<char*>(data), len);

        return data;
    }

    void DurRecoveryUnit::registerChange(Change* change) {
        invariant(inAUnitOfWork());
        _changes.push_back(change);
    }

}  // namespace mongo

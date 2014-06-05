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

#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"

#include "mongo/db/storage/mmap_v1/dur.h"

// Remove once we are ready to enable
#define ROLLBACK_ENABLED 0

namespace mongo {

    DurRecoveryUnit::DurRecoveryUnit() {
        _hasWrittenSinceCheckpoint = false;
    }

    void DurRecoveryUnit::beginUnitOfWork() {
#if ROLLBACK_ENABLED
        _nestingLevel++;
#endif
    }

    void DurRecoveryUnit::commitUnitOfWork() {
#if ROLLBACK_ENABLED
        invariant(_state != MUST_ROLLBACK);
        invariant(_nestingLevel > 0);

        if (_nestingLevel != 1) {
            // If we are nested, punt to outer UnitOfWork. These changes will only be pushed to the
            // global damages list when the outer UnitOfWork commits (which it must now do).
            if (haveUncommitedChanges())
                _state = MUST_COMMIT;
            return;
        }

        publishChanges();
#endif

        // global journal flush
        getDur().commitIfNeeded();
    }

    void DurRecoveryUnit::endUnitOfWork() {
#if ROLLBACK_ENABLED
        invariant(_nestingLevel > 0);

        if (--_nestingLevel != 0) {
            // If we are nested, punt to outer UnitOfWork. These changes will only be rolled back
            // when the outer UnitOfWork rolls back (which it must now do).
            if (haveUncommitedChanges()) {
                invariant(_state != MUST_COMMIT);
                _state = MUST_ROLLBACK;
            }
            return;
        }

        rollbackChanges();
#endif
    }

    void DurRecoveryUnit::publishChanges() {
        invariant(_state != MUST_ROLLBACK);

        if (getDur().isDurable()) {
            for (Changes::iterator it=_changes.begin(), end=_changes.end(); it != end; ++it) {
                // TODO don't go through getDur() interface.
                getDur().writingPtr(it->base, it->preimage.size());
            }
        }

        reset();
    }

    void DurRecoveryUnit::rollbackChanges() {
        invariant(_state != MUST_COMMIT);

        for (Changes::reverse_iterator it=_changes.rbegin(), end=_changes.rend(); it != end; ++it) {
            // TODO need to add these pages to our "dirty count" somehow.
            it->preimage.copy(it->base, it->preimage.size());
        }

        reset();
    }

    void DurRecoveryUnit::recordPreimage(char* data, size_t len) {
        invariant(len > 0);

        Change change;
        change.base = data;
        change.preimage.assign(data, len);
        _changes.push_back(change);
    }

    void DurRecoveryUnit::reset() {
        _state = NORMAL;
        _changes.clear();
    }

    bool DurRecoveryUnit::awaitCommit() {
#if ROLLBACK_ENABLED
        invariant(_state != MUST_ROLLBACK);
        publishChanges();
        _state = NORMAL;
#endif
        return getDur().awaitCommit();
    }

    bool DurRecoveryUnit::commitIfNeeded(bool force) {
#if ROLLBACK_ENABLED
        invariant(_state != MUST_ROLLBACK);
        publishChanges();
#endif
        _hasWrittenSinceCheckpoint = false;
        return getDur().commitIfNeeded(force);
    }

    bool DurRecoveryUnit::isCommitNeeded() const {
        return getDur().isCommitNeeded();
    }

    void* DurRecoveryUnit::writingPtr(void* data, size_t len) {
#if ROLLBACK_ENABLED
        invariant(_nestingLevel >= 1);
        invariant(_state != MUST_ROLLBACK);
        recordPreimage(static_cast<char*>(data), len);
        _hasWrittenSinceCheckpoint = true;
        return data;
#else
        _hasWrittenSinceCheckpoint = true;
        return getDur().writingPtr(data, len);
#endif
    }

    void DurRecoveryUnit::syncDataAndTruncateJournal() {
#if ROLLBACK_ENABLED
        invariant(_state != MUST_ROLLBACK);
        publishChanges();
#endif
        return getDur().syncDataAndTruncateJournal();
    }

}  // namespace mongo

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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"

#include <algorithm>
#include <string>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/dur.h"

// Remove once we are ready to enable
#define ROLLBACK_ENABLED 0

namespace mongo {


    /**
     *  A MemoryWrite provides rollback by keeping a pre-image.
     */
    class MemoryWrite : public RecoveryUnit::Change {
    public:
        MemoryWrite(char* base, size_t len) : _base(base), _preimage(base, len) { }
        virtual void commit();
        virtual void rollback();

    private:
        char* _base;
        std::string _preimage;  // TODO consider storing out-of-line
    };

    DurRecoveryUnit::DurRecoveryUnit(OperationContext* txn)
        : _txn(txn),
          _state(NORMAL)
    {}

    void DurRecoveryUnit::beginUnitOfWork() {
#if ROLLBACK_ENABLED
        _startOfUncommittedChangesForLevel.push_back(_changes.size());
#endif
    }

    void DurRecoveryUnit::commitUnitOfWork() {
#if ROLLBACK_ENABLED
        invariant(inAUnitOfWork());

        if (!inOutermostUnitOfWork()) {
            // If we are nested, make all changes for this level part of the containing UnitOfWork.
            // They will be added to the global damages list once the outermost UnitOfWork commits,
            // which it must now do.
            if (haveUncommitedChangesAtCurrentLevel()) {
                _startOfUncommittedChangesForLevel.back() = _changes.size();
                _state = MUST_COMMIT;
            }
            return;
        }

        publishChanges();
#endif

        // global journal flush opportunity
        getDur().commitIfNeeded(_txn);
    }

    void DurRecoveryUnit::endUnitOfWork() {
#if ROLLBACK_ENABLED
        invariant(inAUnitOfWork());

        if (haveUncommitedChangesAtCurrentLevel()) {
            invariant(_state != MUST_COMMIT);
            rollbackInnermostChanges();
        }

        // If outermost, we return to "normal" state after rolling back.
        if (inOutermostUnitOfWork())
            _state = NORMAL;

        _startOfUncommittedChangesForLevel.pop_back();
#endif
    }

    void DurRecoveryUnit::publishChanges() {
        for (Changes::iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }

        // We now reset to a "clean" state without any uncommited changes, while keeping the same
        // nesting level. Eventually this should only be called from the outermost UnitOfWork.
        _state = NORMAL;
        _changes.clear();
        std::fill(_startOfUncommittedChangesForLevel.begin(),
                  _startOfUncommittedChangesForLevel.end(),
                  0);
    }

    void DurRecoveryUnit::rollbackInnermostChanges() {
        invariant(_state != MUST_COMMIT);

        invariant(_changes.size() <= size_t(std::numeric_limits<int>::max()));
        const int rollbackTo = _startOfUncommittedChangesForLevel.back();
        for (int i = _changes.size() - 1; i >= rollbackTo; i--) {
            _changes[i]->rollback();
        }
        _changes.erase(_changes.begin() + rollbackTo, _changes.end());
    }

    void DurRecoveryUnit::recordPreimage(char* data, size_t len) {
        invariant(len > 0);

        registerChange(new MemoryWrite(data, len));
    }

    bool DurRecoveryUnit::awaitCommit() {
#if ROLLBACK_ENABLED
        // TODO this is currently only called outside of WriteLocks and UnitsOfWork.
        // Consider enforcing with an invariant rather than correctly handling uncommitted changes.
        publishChanges();
        _state = NORMAL;
#endif
        return getDur().awaitCommit();
    }

    bool DurRecoveryUnit::commitIfNeeded(bool force) {
        // TODO see if we can ban this inside of nested UnitsOfWork.
#if ROLLBACK_ENABLED
        publishChanges();
#endif
        return getDur().commitIfNeeded(_txn, force);
    }

    bool DurRecoveryUnit::isCommitNeeded() const {
        return getDur().isCommitNeeded();
    }

    void* DurRecoveryUnit::writingPtr(void* data, size_t len) {
        invariant(len > 0);
#if ROLLBACK_ENABLED
        invariant(inAUnitOfWork());
        // TODO need to call MemoryMappedFile::makeWritable()
        registerChange(new MemoryWrite(static_cast<char*>(data), len));
        return data;
#else
        invariant(_txn->lockState()->isWriteLocked());

        return getDur().writingPtr(data, len);
#endif
    }

    void DurRecoveryUnit::registerChange(Change* change) {
#if ROLLBACK_ENABLED
        invariant(inAUnitOfWork());
        _changes.push_back(ChangePtr(change));
#else
        change->commit();
        delete change;
#endif
    }

    void DurRecoveryUnit::syncDataAndTruncateJournal() {
#if ROLLBACK_ENABLED
        publishChanges();
#endif
        return getDur().syncDataAndTruncateJournal(_txn);
    }

    void MemoryWrite::commit() {
        // TODO don't go through getDur() interface.
        if (getDur().isDurable()) {
            getDur().writingPtr(_base, _preimage.size());
        }
    }

    void MemoryWrite::rollback() {
        // TODO need to add these pages to our "dirty count" somehow.
       _preimage.copy(_base, _preimage.size());
    }

}  // namespace mongo

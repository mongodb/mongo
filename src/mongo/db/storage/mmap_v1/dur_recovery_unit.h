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

#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/compiler.h"

#pragma once

namespace mongo {

    /**
     * Just pass through to getDur().
     */
    class DurRecoveryUnit : public RecoveryUnit {
    public:
        DurRecoveryUnit();

        virtual ~DurRecoveryUnit() { }

        virtual void beginUnitOfWork(OperationContext* opCtx);
        virtual void commitUnitOfWork();
        virtual void endUnitOfWork();

        virtual bool awaitCommit();

        virtual void commitAndRestart();

        //  The recovery unit takes ownership of change.
        virtual void registerChange(Change* change);

        virtual void* writingPtr(void* data, size_t len);

        virtual void setRollbackWritesDisabled();

    private:
        void commitChanges();
        void pushChangesToDurSubSystem();
        void rollbackInnermostChanges();

        bool inAUnitOfWork() const { return !_startOfUncommittedChangesForLevel.empty(); }

        bool inOutermostUnitOfWork() const {
            return _startOfUncommittedChangesForLevel.size() == 1;
        }

        bool haveUncommitedChangesAtCurrentLevel() const {
            return _writes.size() > _startOfUncommittedChangesForLevel.back().writeIndex
                || _changes.size() > _startOfUncommittedChangesForLevel.back().changeIndex;
        }

        // Changes are ordered from oldest to newest.
        typedef OwnedPointerVector<Change> Changes;
        Changes _changes;

        // These are memory writes inside the mmapv1 mmaped files. Writes are ordered from oldest to
        // newest. Overlapping and duplicate regions are allowed, since rollback undoes changes in
        // reverse order.
        std::string _preimageBuffer;
        struct Write {
            Write(char* addr, int len, int offset) : addr(addr), len(len), offset(offset) {}

            bool operator < (const Write& rhs) const { return addr < rhs.addr; }

            char* addr;
            int len;
            int offset; // index into _preimageBuffer;
        };
        typedef std::vector<Write> Writes;
        Writes _writes;

        // Indexes of the first uncommitted Change/Write in _changes/_writes for each nesting level.
        // Index 0 in this vector is always the outermost transaction and back() is always the
        // innermost. The size() is the current nesting level.
        struct Indexes {
            Indexes(size_t changeIndex, size_t writeIndex)
                : changeIndex(changeIndex)
                , writeIndex(writeIndex)
            {}
            size_t changeIndex;
            size_t writeIndex;
        };
        std::vector<Indexes> _startOfUncommittedChangesForLevel;

        // If true, this RU is in a "failed" state and all changes must be rolled back. Once the
        // outermost WUOW rolls back it reverts to false.
        bool _mustRollback;

        // Default is false.  
        // If true, no preimages are tracked.  If rollback is subsequently attempted, the process
        // will abort.
        bool _rollbackDisabled;
    };

}  // namespace mongo

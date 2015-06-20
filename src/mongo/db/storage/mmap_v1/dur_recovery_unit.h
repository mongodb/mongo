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

#include <set>
#include <string>
#include <utility>
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

    void beginUnitOfWork(OperationContext* opCtx) final;
    void commitUnitOfWork() final;
    void abortUnitOfWork() final;

    virtual bool waitUntilDurable();

    virtual void abandonSnapshot();

    //  The recovery unit takes ownership of change.
    virtual void registerChange(Change* change);

    virtual void* writingPtr(void* addr, size_t len);

    virtual void setRollbackWritesDisabled();

    virtual SnapshotId getSnapshotId() const {
        return SnapshotId();
    }

private:
    /**
     * Marks writes for journaling, if enabled, and then commits all other Changes in order.
     * Returns with empty _initialWrites, _mergedWrites, _changes and _preimageBuffer, but
     * does not reset the _rollbackWritesDisabled or _mustRollback flags. This leaves the
     * RecoveryUnit ready for more changes that may be committed or rolled back.
     */
    void commitChanges();

    /**
     * Creates a list of write intents to be journaled, and hands it of to the active
     * DurabilityInterface.
     */
    void markWritesForJournaling();

    /**
     * Restores state by rolling back all writes using the saved pre-images, and then
     * rolling back all other Changes in LIFO order. Resets internal state.
     */
    void rollbackChanges();


    /**
     * Version of writingPtr that checks existing writes for overlap and only stores those
     * changes not yet covered by an existing write intent and pre-image.
     */
    void mergingWritingPtr(char* data, size_t len);

    /**
     * Reset to a clean state without any uncommitted changes or write.
     */
    void resetChanges();

    // Changes are ordered from oldest to newest.
    typedef OwnedPointerVector<Change> Changes;
    Changes _changes;


    // Number of pending uncommitted writes. Incremented even if new write is fully covered by
    // existing writes.
    size_t _writeCount;
    // Total size of the pending uncommitted writes.
    size_t _writeBytes;

    /**
     * These are memory writes inside the mmapv1 mmap-ed files. A pointer past the end is just
     * instead of a pointer to the beginning for the benefit of MergedWrites.
     */
    struct Write {
        Write(char* addr, int len, int offset) : addr(addr), len(len), offset(offset) {}
        Write(const Write& rhs) : addr(rhs.addr), len(rhs.len), offset(rhs.offset) {}
        Write() : addr(0), len(0), offset(0) {}
        bool operator<(const Write& rhs) const {
            return addr < rhs.addr;
        }

        struct compareEnd {
            bool operator()(const Write& lhs, const Write& rhs) const {
                return lhs.addr + lhs.len < rhs.addr + rhs.len;
            }
        };

        char* end() const {
            return addr + len;
        }

        char* addr;
        int len;
        int offset;  // index into _preimageBuffer
    };

    /**
     * Writes are ordered by ending address, so MergedWrites::upper_bound() can find the first
     * overlapping write, if any. Overlapping and duplicate regions are forbidden, as rollback
     * of MergedChanges undoes changes by address rather than LIFO order. In addition, empty
     * regions are not allowed. Storing writes by age does not work well for large indexed
     * arrays, as coalescing is needed to bound the size of the preimage buffer.
     */
    typedef std::set<Write, Write::compareEnd> MergedWrites;
    MergedWrites _mergedWrites;

    // Generally it's more efficient to just store pre-images unconditionally and then
    // sort/eliminate duplicates at commit time. However, this can lead to excessive memory
    // use in cases involving large indexes arrays, where the same memory is written many
    // times. To keep the speed for the general case and bound memory use, the first few MB of
    // pre-images are stored unconditionally, but once the threshold has been exceeded, the
    // remainder is stored in a more space-efficient datastructure.
    typedef std::vector<Write> InitialWrites;
    InitialWrites _initialWrites;

    std::string _preimageBuffer;

    bool _inUnitOfWork;


    // Default is false.
    // If true, no preimages are tracked.  If rollback is subsequently attempted, the process
    // will abort.
    bool _rollbackWritesDisabled;
};

}  // namespace mongo

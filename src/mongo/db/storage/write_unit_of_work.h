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

#pragma once

#include <iosfwd>
#include <memory>

namespace mongo {

class OperationContext;

/**
 * The WriteUnitOfWork is an RAII type that begins a storage engine write unit of work on both the
 * Locker and the RecoveryUnit of the OperationContext. Any writes that occur during the lifetime of
 * this object will be committed when commit() is called, and rolled back (aborted) when the object
 * is destructed without a call to commit() or release().
 *
 * A WriteUnitOfWork can be nested with others, but only the top level WriteUnitOfWork will commit
 * the unit of work on the RecoveryUnit. If a low level WriteUnitOfWork aborts, any parents will
 * also abort.
 *
 * The WriteUnitOfWork may be used in read only mode, where it allows callbacks to be registered
 * with the RecoveryUnit and executes them on commit/abort without opening any unit of work on the
 * RecoveryUnit. This can be used to unify code that performs in-memory writes using the callback
 * functionality of the RecoveryUnit.
 */
class WriteUnitOfWork {
    WriteUnitOfWork(const WriteUnitOfWork&) = delete;
    WriteUnitOfWork& operator=(const WriteUnitOfWork&) = delete;

public:
    /**
     * The RecoveryUnitState is used to ensure valid state transitions.
     */
    enum RecoveryUnitState {
        kNotInUnitOfWork,   // not in a unit of work, no writes allowed
        kActiveUnitOfWork,  // in a unit of work that still may either commit or abort
        kFailedUnitOfWork   // in a unit of work that has failed and must be aborted
    };

    enum OplogEntryGroupType {
        kDontGroup,
        kGroupForTransaction,
        kGroupForPossiblyRetryableOperations
    };

    WriteUnitOfWork(OperationContext* opCtx, OplogEntryGroupType groupType = kDontGroup);

    ~WriteUnitOfWork();

    /**
     * Creates a top-level WriteUnitOfWork without changing RecoveryUnit or Locker state. For use
     * when the RecoveryUnit and Locker are in active or failed state.
     */
    static std::unique_ptr<WriteUnitOfWork> createForSnapshotResume(OperationContext* opCtx,
                                                                    RecoveryUnitState ruState);

    /**
     * Releases the OperationContext RecoveryUnit and Locker objects from management without
     * changing state. Allows for use of these objects beyond the WriteUnitOfWork lifespan. Prepared
     * units of work are not allowed be released. Returns the state of the RecoveryUnit.
     */
    RecoveryUnitState release();

    /**
     * Transitions the WriteUnitOfWork to the "prepared" state. The RecoveryUnit state in the
     * OperationContext must be active. The WriteUnitOfWork may not be nested and will invariant in
     * that case. Will throw CommandNotSupported if the storage engine does not support prepared
     * transactions. May throw WriteConflictException.
     *
     * No subsequent operations are allowed except for commit or abort (when the object is
     * destructed).
     */
    void prepare();

    /**
     * Commits the WriteUnitOfWork. If this is the top level unit of work, the RecoveryUnit's unit
     * of work is committed. Commit can only be called once on an active unit of work, and may not
     * be called on a released WriteUnitOfWork.
     */
    void commit();

private:
    WriteUnitOfWork() = default;  // for createForSnapshotResume

    /**
     * Whether this WUOW is grouping oplog entries, regardless of the grouping type.
     */
    bool _isGroupingOplogEntries() {
        return _groupOplogEntries != kDontGroup;
    }

    OperationContext* _opCtx;

    bool _toplevel;
    OplogEntryGroupType _groupOplogEntries;

    bool _committed = false;
    bool _prepared = false;
    bool _released = false;
};

std::ostream& operator<<(std::ostream& os, WriteUnitOfWork::RecoveryUnitState state);

}  // namespace mongo

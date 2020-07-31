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

#include <functional>
#include <vector>

#include "mongo/db/record_id.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {
namespace ephemeral_for_test {

class RecoveryUnit : public ::mongo::RecoveryUnit {
public:
    RecoveryUnit(KVEngine* parentKVEngine, std::function<void()> cb = nullptr);
    ~RecoveryUnit();

    void beginUnitOfWork(OperationContext* opCtx) override final;

    bool inActiveTxn() const {
        return _inUnitOfWork();
    }

    virtual bool waitUntilDurable(OperationContext* opCtx) override;

    virtual void setOrderedCommit(bool orderedCommit) override;

    Status obtainMajorityCommittedSnapshot() final;

    void prepareUnitOfWork() override;

    virtual void setPrepareTimestamp(Timestamp ts) override {
        _prepareTimestamp = ts;
    }

    virtual Timestamp getPrepareTimestamp() const override {
        return _prepareTimestamp;
    }

    virtual void setCommitTimestamp(Timestamp ts) override {
        _commitTimestamp = ts;
    }

    virtual Timestamp getCommitTimestamp() const override {
        return _commitTimestamp;
    }

    virtual void clearCommitTimestamp() override {
        _commitTimestamp = Timestamp::min();
    }

    Status setTimestamp(Timestamp timestamp) override;

    void setTimestampReadSource(ReadSource readSource,
                                boost::optional<Timestamp> provided) override;

    ReadSource getTimestampReadSource() const override;

    // Ephemeral for test specific function declarations below.
    StringStore* getHead() {
        forkIfNeeded();
        return &_workingCopy;
    }

    inline void makeDirty() {
        _dirty = true;
    }

    /**
     * Checks if there already exists a current working copy and merge base; if not fetches
     * one and creates them.
     */
    bool forkIfNeeded();

    static RecoveryUnit* get(OperationContext* opCtx);

private:
    void doCommitUnitOfWork() override final;

    void doAbortUnitOfWork() override final;

    void doAbandonSnapshot() override final;

    void _abort();

    void _setMergeNull();

    std::function<void()> _waitUntilDurableCallback;
    // Official master is kept by KVEngine
    KVEngine* _KVEngine;
    // We need _mergeBase to be a shared_ptr to hold references in KVEngine::_availableHistory.
    // _mergeBase will be initialized in forkIfNeeded().
    std::shared_ptr<StringStore> _mergeBase;
    // We need _workingCopy to be a unique copy, not a shared_ptr.
    StringStore _workingCopy;

    bool _forked = false;
    bool _dirty = false;  // Whether or not we have written to this _workingCopy.

    Timestamp _prepareTimestamp = Timestamp::min();
    Timestamp _commitTimestamp = Timestamp::min();

    // Specifies which external source to use when setting read timestamps on transactions.
    ReadSource _timestampReadSource = ReadSource::kUnset;
    boost::optional<Timestamp> _readAtTimestamp = boost::none;
};

}  // namespace ephemeral_for_test
}  // namespace mongo

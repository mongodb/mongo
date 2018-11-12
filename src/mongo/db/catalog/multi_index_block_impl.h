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

#include "mongo/db/catalog/multi_index_block.h"

#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class BackgroundOperation;
class BSONObj;
class Collection;
class OperationContext;

class MultiIndexBlockImpl : public MultiIndexBlock {
    MONGO_DISALLOW_COPYING(MultiIndexBlockImpl);

public:
    MultiIndexBlockImpl(OperationContext* opCtx, Collection* collection);
    ~MultiIndexBlockImpl() override;

    void allowBackgroundBuilding() override {
        _buildInBackground = true;
    }

    void allowInterruption() override {
        _allowInterruption = true;
    }

    void ignoreUniqueConstraint() override {
        _ignoreUnique = true;
    }

    void removeExistingIndexes(std::vector<BSONObj>* specs) const override;

    StatusWith<std::vector<BSONObj>> init(const std::vector<BSONObj>& specs) override;
    StatusWith<std::vector<BSONObj>> init(const BSONObj& spec) override;

    Status insertAllDocumentsInCollection() override;

    Status insert(const BSONObj& doc,
                  const RecordId& loc,
                  std::vector<BSONObj>* const dupKeysInserted = nullptr) override;

    Status dumpInsertsFromBulk() override;
    Status dumpInsertsFromBulk(std::set<RecordId>* dupRecords) override;
    Status dumpInsertsFromBulk(std::vector<BSONObj>* dupKeysInserted) override;

    /**
     * See MultiIndexBlock::drainBackgroundWritesIfNeeded()
     */
    Status drainBackgroundWritesIfNeeded() override;

    Status commit() override;
    Status commit(stdx::function<void(const BSONObj& spec)> onCreateFn) override;

    bool isCommitted() const override;

    void abort(StringData reason) override;

    void abortWithoutCleanup() override;

    bool getBuildInBackground() const override {
        return _buildInBackground;
    }

    /**
     * State transitions:
     *
     * Uninitialized --> Running --> PreCommit --> Committed
     *       |              |            |            ^
     *       |              |            |            |
     *       \--------------+------------+-------> Aborted
     *
     * It is possible for abort() to skip intermediate states. For example, calling abort() when the
     * index build has not been initialized will transition from Uninitialized directly to Aborted.
     *
     * In the case where we are in the midst of committing the WUOW for a successful commit() call,
     * we may transition temporarily to Aborted before finally ending at Committed. See comments for
     * MultiIndexBlock::abort().
     *
     * Not part of MultiIndexBlock interface. Callers should not have to query the state of the
     * MultiIndexBlock directly.
     */
    enum class State { kUninitialized, kRunning, kPreCommit, kCommitted, kAborted };
    State getState_forTest() const;

private:
    class SetNeedToCleanupOnRollback;
    class CleanupIndexesVectorOnRollback;

    struct IndexToBuild {
        std::unique_ptr<IndexCatalog::IndexBuildBlockInterface> block;

        IndexAccessMethod* real = NULL;           // owned elsewhere
        const MatchExpression* filterExpression;  // might be NULL, owned elsewhere
        std::unique_ptr<IndexAccessMethod::BulkBuilder> bulk;

        InsertDeleteOptions options;
    };

    Status _dumpInsertsFromBulk(std::set<RecordId>* dupRecords,
                                std::vector<BSONObj>* dupKeysInserted);

    /**
     * Returns the current state.
     */
    State _getState() const;

    /**
     * Updates the current state to a non-Aborted state.
     */
    void _setState(State newState);

    /**
     * Updates the current state to Aborted with the given reason.
     */
    void _setStateToAbortedIfNotCommitted(StringData reason);

    std::vector<IndexToBuild> _indexes;

    std::unique_ptr<BackgroundOperation> _backgroundOperation;

    // Pointers not owned here and must outlive 'this'
    Collection* _collection;
    OperationContext* _opCtx;

    bool _buildInBackground;
    bool _allowInterruption;
    bool _ignoreUnique;

    bool _needToCleanup;

    // Protects member variables of this class declared below.
    mutable stdx::mutex _mutex;

    State _state = State::kUninitialized;
    std::string _abortReason;
};

// For unit tests that need to check MultiIndexBlock states.
// The ASSERT_*() macros use this function to print the value of 'state' when the predicate fails.
std::ostream& operator<<(std::ostream& os, const MultiIndexBlockImpl::State& state);

}  // namespace mongo

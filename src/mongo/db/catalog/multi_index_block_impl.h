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

namespace mongo {

class BackgroundOperation;
class BSONObj;
class Collection;
class OperationContext;

class MultiIndexBlockImpl : public MultiIndexBlock {
    MONGO_DISALLOW_COPYING(MultiIndexBlockImpl);

public:
    /**
     * Neither pointer is owned.
     */
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

    Status doneInserting() override;
    Status doneInserting(std::set<RecordId>* dupRecords) override;
    Status doneInserting(std::vector<BSONObj>* dupKeysInserted) override;

    Status commit() override;
    Status commit(stdx::function<void(const BSONObj& spec)> onCreateFn) override;

    void abortWithoutCleanup() override;

    bool getBuildInBackground() const override {
        return _buildInBackground;
    }

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

    Status _doneInserting(std::set<RecordId>* dupRecords, std::vector<BSONObj>* dupKeysInserted);

    std::vector<IndexToBuild> _indexes;

    std::unique_ptr<BackgroundOperation> _backgroundOperation;

    // Pointers not owned here and must outlive 'this'
    Collection* _collection;
    OperationContext* _opCtx;

    bool _buildInBackground;
    bool _allowInterruption;
    bool _ignoreUnique;

    bool _needToCleanup;
};

}  // namespace mongo

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
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace repl {

/**
 * Class in charge of building a collection during data loading (like initial sync).
 *
 * Note: Call commit when done inserting documents.
 */
class CollectionBulkLoaderImpl : public CollectionBulkLoader {
    CollectionBulkLoaderImpl(const CollectionBulkLoaderImpl&) = delete;
    CollectionBulkLoaderImpl& operator=(const CollectionBulkLoaderImpl&) = delete;

public:
    struct Stats {
        Date_t startBuildingIndexes;
        Date_t endBuildingIndexes;

        std::string toString() const;
        BSONObj toBSON() const;
    };

    CollectionBulkLoaderImpl(ServiceContext::UniqueClient client,
                             ServiceContext::UniqueOperationContext opCtx,
                             const NamespaceString& nss);
    ~CollectionBulkLoaderImpl() override;

    Status init(const BSONObj& idIndexSpec, const std::vector<BSONObj>& secondaryIndexSpecs);

    Status insertDocuments(std::span<BSONObj> objs, ParseRecordIdAndDocFunc fn) override;
    Status commit() override;

    CollectionBulkLoaderImpl::Stats getStats() const;

private:
    void _releaseResources();

    template <typename F>
    Status _runTaskReleaseResourcesOnFailure(const F& task) noexcept;

    /**
     * Adds document and associated RecordId to index blocks after inserting into RecordStore.
     */
    Status _addDocumentToIndexBlocks(const BSONObj& doc, const RecordId& loc);

    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    CollectionAcquisition _acquisition;
    NamespaceString _nss;
    std::unique_ptr<MultiIndexBlock> _idIndexBlock;
    std::unique_ptr<MultiIndexBlock> _secondaryIndexesBlock;
    Stats _stats;
};

}  // namespace repl
}  // namespace mongo

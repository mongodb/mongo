// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
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

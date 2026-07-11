// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace analyze_shard_key {

/**
 * OpObserver for query analysis in a replica set.
 */
class [[MONGO_MOD_PUBLIC]] QueryAnalysisOpObserverRS final : public QueryAnalysisOpObserver {
    QueryAnalysisOpObserverRS(const QueryAnalysisOpObserverRS&) = delete;
    QueryAnalysisOpObserverRS& operator=(const QueryAnalysisOpObserverRS&) = delete;

public:
    QueryAnalysisOpObserverRS() = default;
    ~QueryAnalysisOpObserverRS() override = default;

    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kAll, NamespaceFilter::kConfig};
    }

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;
};

}  // namespace analyze_shard_key
}  // namespace mongo

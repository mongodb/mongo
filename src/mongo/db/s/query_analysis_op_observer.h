// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace analyze_shard_key {

/**
 * OpObserver for query analysis.
 */
class QueryAnalysisOpObserver : public OpObserverNoop {
    QueryAnalysisOpObserver(const QueryAnalysisOpObserver&) = delete;
    QueryAnalysisOpObserver& operator=(const QueryAnalysisOpObserver&) = delete;

public:
    QueryAnalysisOpObserver() = default;
    ~QueryAnalysisOpObserver() override = default;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override = 0;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override = 0;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override = 0;

protected:
    void insertInConfigQueryAnalyzersNamespaceImpl(
        OperationContext* opCtx,
        std::vector<InsertStatement>::const_iterator begin,
        std::vector<InsertStatement>::const_iterator end);
    void updateToConfigQueryAnalyzersNamespaceImpl(OperationContext* opCtx,
                                                   const OplogUpdateEntryArgs& args);
    void updateWithSampleIdImpl(OperationContext* opCtx, const OplogUpdateEntryArgs& args);
    void deleteFromConfigQueryAnalyzersNamespaceImpl(OperationContext* opCtx,
                                                     const OplogDeleteEntryArgs& args,
                                                     const BSONObj& doc);
};

}  // namespace analyze_shard_key
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/query_analysis_op_observer_rs.h"

#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

void QueryAnalysisOpObserverRS::onInserts(OperationContext* opCtx,
                                          const CollectionPtr& coll,
                                          std::vector<InsertStatement>::const_iterator begin,
                                          std::vector<InsertStatement>::const_iterator end,
                                          const std::vector<RecordId>& recordIds,
                                          std::vector<bool> fromMigrate,
                                          bool defaultFromMigrate,
                                          OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
        insertInConfigQueryAnalyzersNamespaceImpl(opCtx, begin, end);
    }
}

void QueryAnalysisOpObserverRS::onUpdate(OperationContext* opCtx,
                                         const OplogUpdateEntryArgs& args,
                                         OpStateAccumulator* opAccumulator) {
    if (args.coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
        updateToConfigQueryAnalyzersNamespaceImpl(opCtx, args);
    }

    if (args.updateArgs->sampleId && opCtx->writesAreReplicated()) {
        updateWithSampleIdImpl(opCtx, args);
    }
}

void QueryAnalysisOpObserverRS::onDelete(OperationContext* opCtx,
                                         const CollectionPtr& coll,
                                         StmtId stmtId,
                                         const BSONObj& doc,
                                         const DocumentKey& documentKey,
                                         const OplogDeleteEntryArgs& args,
                                         OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
        invariant(!doc.isEmpty());
        deleteFromConfigQueryAnalyzersNamespaceImpl(opCtx, args, doc);
    }
}
}  // namespace analyze_shard_key
}  // namespace mongo

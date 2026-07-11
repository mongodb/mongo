// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/query_analysis_op_observer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/util/future.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

void QueryAnalysisOpObserver::insertInConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator begin,
    std::vector<InsertStatement>::const_iterator end) {
    for (auto it = begin; it != end; ++it) {
        auto parsedDoc = QueryAnalyzerDocument::parse(
            it->doc, IDLParserContext("QueryAnalysisOpObserver::onInserts"));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx,
                                               boost::optional<Timestamp>) {
                analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationInsert(
                    parsedDoc);
            });
    }
}

void QueryAnalysisOpObserver::updateToConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    auto parsedDoc = QueryAnalyzerDocument::parse(
        args.updateArgs->updatedDoc, IDLParserContext("QueryAnalysisOpObserver::onUpdate"));
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx, boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationUpdate(
                parsedDoc);
        });
}

void QueryAnalysisOpObserver::updateWithSampleIdImpl(OperationContext* opCtx,
                                                     const OplogUpdateEntryArgs& args) {
    analyze_shard_key::QueryAnalysisWriter::get(opCtx)
        ->addDiff(*args.updateArgs->sampleId,
                  args.coll->ns(),
                  args.coll->uuid(),
                  args.updateArgs->preImageDoc,
                  args.updateArgs->updatedDoc)
        .getAsync([](auto) {});
}

void QueryAnalysisOpObserver::deleteFromConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx, const OplogDeleteEntryArgs& args, const BSONObj& doc) {
    auto parsedDoc =
        QueryAnalyzerDocument::parse(doc, IDLParserContext("QueryAnalysisOpObserver::onDelete"));
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx, boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationDelete(
                parsedDoc);
        });
}

}  // namespace analyze_shard_key
}  // namespace mongo

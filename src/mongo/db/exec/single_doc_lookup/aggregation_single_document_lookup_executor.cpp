// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"

#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/util/timer.h"

namespace mongo::exec::agg {

SingleDocumentLookupExecutor::LookupResult AggregationSingleDocumentLookupExecutor::performLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<Timestamp> afterClusterTime) {
    // Build the readConcern only here, on the routed path that actually consumes it.
    // 'afterClusterTime' makes the remote read return a majority snapshot at or after the event's
    // clusterTime.
    boost::optional<BSONObj> readConcern;
    if (afterClusterTime) {
        readConcern = BSON("level" << "majority"
                                   << "afterClusterTime" << *afterClusterTime);
    }

    // Time the lookup itself. A throw (e.g. TooManyMatchingDocuments) records nothing: the outcome
    // is neither found nor not-found, and the timer simply unwinds.
    Timer timer;
    auto result = expCtx->getMongoProcessInterface()->lookupSingleDocument(
        expCtx, nss, std::move(collectionUUID), documentKey, std::move(readConcern));

    if (result) {
        _recorder.recordFound(timer.elapsed());
        return {LookupResult::HandledStatus::kDocumentFound, std::move(*result)};
    }
    _recorder.recordNotFound(timer.elapsed());
    return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
}

}  // namespace mongo::exec::agg

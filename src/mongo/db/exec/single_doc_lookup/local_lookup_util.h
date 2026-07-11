// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {

/**
 * Builds the {_id: <value>} equality filter from the "_id" field, materializing only that field.
 * Returns boost::none when the documentKey carries no _id.
 */
inline boost::optional<BSONObj> makeIdEqualityFilter(const Document& documentKey) {
    const Value idVal = documentKey["_id"];
    if (idVal.missing()) {
        return boost::none;
    }
    BSONObjBuilder filterBuilder;
    idVal.addToBsonObj(&filterBuilder, "_id");
    return filterBuilder.obj();
}

/**
 * Publishes a single lookup's index usage to the collection's $indexStats counters, and folds the
 * summary's index-usage fields into 'statsSink' when one is set (no-op otherwise).
 */
inline void recordIndexUsage(const Collection* rawCollPtr,
                             const PlanSummaryStats& summary,
                             PlanSummaryStats* statsSink) {
    CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
        rawCollPtr,
        summary.collectionScans,
        summary.collectionScansNonTailable,
        summary.indexesUsed);

    if (statsSink) {
        statsSink->nReturned += summary.nReturned;
        statsSink->totalKeysExamined += summary.totalKeysExamined;
        statsSink->totalDocsExamined += summary.totalDocsExamined;
        statsSink->indexesUsed.insert(summary.indexesUsed.begin(), summary.indexesUsed.end());
    }
}

/**
 * Runs 'body' and maps a collection dropped or renamed mid-lookup to kDocumentNotFound.
 */
template <typename F>
SingleDocumentLookupExecutor::LookupResult withCollectionGoneMappedToNotFound(F&& body) {
    using LookupResult = SingleDocumentLookupExecutor::LookupResult;
    try {
        return body();
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2_DEBUG(
            12952810, 1, "Namespace not found during local lookup", "error"_attr = redact(ex));
        return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
    } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
        LOGV2_DEBUG(
            12952811, 1, "Collection UUID mismatch during local lookup", "error"_attr = redact(ex));
        return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
    }
}

}  // namespace mongo::exec::agg

#undef MONGO_LOGV2_DEFAULT_COMPONENT

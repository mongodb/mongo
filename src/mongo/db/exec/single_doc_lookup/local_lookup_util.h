/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

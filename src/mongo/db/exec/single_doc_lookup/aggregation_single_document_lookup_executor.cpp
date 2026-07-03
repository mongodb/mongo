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

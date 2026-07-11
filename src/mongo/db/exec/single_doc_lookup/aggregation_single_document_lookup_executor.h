// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"

namespace mongo::exec::agg {

/**
 * Universal fallback lookup strategy that delegates to the process interface's
 * lookupSingleDocument(). Handles all topologies including remote routing on sharded clusters.
 *
 * Never returns kNotHandled: this is the guaranteed fallback that the chain terminates in.
 */
class AggregationSingleDocumentLookupExecutor : public SingleDocumentLookupExecutor {
public:
    explicit AggregationSingleDocumentLookupExecutor(SingleDocumentLookupStatsRecorder recorder)
        : _recorder(std::move(recorder)) {}

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

private:
    SingleDocumentLookupStatsRecorder _recorder;
};

}  // namespace mongo::exec::agg

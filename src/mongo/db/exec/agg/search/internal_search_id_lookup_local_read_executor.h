// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * idLookup's original lookup strategy, now behind SingleDocumentLookupExecutor: resolves an _id via
 * a `$match`-on-_id sub-pipeline (optionally + the view pipeline) against the stage's stashed
 * acquisition, whose shard filter drops orphans on sharded collections. Installed when the feature
 * flag is off or a view is present; always handles the lookup (never kNotHandled).
 */
class InternalSearchIdLookUpLocalReadExecutor final : public SingleDocumentLookupExecutor {
public:
    InternalSearchIdLookUpLocalReadExecutor(
        boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> catalogResourceHandle,
        boost::optional<std::vector<BSONObj>> viewPipeline)
        : _catalogResourceHandle(std::move(catalogResourceHandle)),
          _viewPipeline(std::move(viewPipeline)) {}

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _planSummaryStatsSink = sink;
    }

private:
    boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> _catalogResourceHandle;
    boost::optional<std::vector<BSONObj>> _viewPipeline;

    // Non-owning; owned by the stage. Stats are accumulated here when set.
    PlanSummaryStats* _planSummaryStatsSink = nullptr;
};

}  // namespace mongo::exec::agg

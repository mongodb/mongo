// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalSearchIdLookUpStage final : public Stage {
public:
    using SearchIdLookupMetrics = DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics;

    InternalSearchIdLookUpStage(
        std::string_view stageName,
        DocumentSourceIdLookupSpec spec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
            catalogResourceHandle,
        const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
        std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor);

    const SpecificStats* getSpecificStats() const override {
        return &_stats;
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const override;

    /**
     * Test-only accessor for the single-document lookup executor held by this stage, so wiring
     * tests can assert the concrete strategy installed by the stage factory via dynamic_cast.
     */
    const SingleDocumentLookupExecutor* getLookupExecutor_forTest() const {
        return _lookupExecutor.get();
    }

private:
    GetNextResult doGetNext() final;

    const std::string _stageName;
    const DocumentSourceIdLookupSpec _spec;

    // Handle on catalog state that can be acquired and released during doGetNext(). Also contains
    // the collection needed for execution.
    boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> _catalogResourceHandle;

    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics;
    DocumentSourceIdLookupStats _stats;

    // Lookup strategy doGetNext() drives for every _id (never null): the Express fast path or the
    // local-read executor, chosen by buildIdLookupExecutor().
    std::unique_ptr<SingleDocumentLookupExecutor> _lookupExecutor;
};

/**
 * Builds the executor InternalSearchIdLookUpStage drives for every _id. idLookup has no remote
 * lookup, so there is no fallback. Returns the Express fast path when the flag is on and there is
 * no view (a view is not a pure _id point lookup); otherwise the local-read executor. The stage
 * wires its explain stats sink through the executor interface, so no separate handle is returned.
 */
std::unique_ptr<SingleDocumentLookupExecutor> buildIdLookupExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
        catalogResourceHandle,
    boost::optional<std::vector<BSONObj>> viewPipeline);

}  // namespace mongo::exec::agg

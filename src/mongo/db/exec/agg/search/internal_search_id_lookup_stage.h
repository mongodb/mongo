/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

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

    // Express fast-path strategy for local _id point lookups. Held by the stage but, as of this
    // change, not yet consulted by doGetNext(); wiring it into execution behind a feature flag is a
    // follow-up.
    std::unique_ptr<SingleDocumentLookupExecutor> _lookupExecutor;
};

/**
 * Builds the Express SingleDocumentLookupExecutor that InternalSearchIdLookUpStage uses to resolve
 * each _id to its full document.
 */
std::unique_ptr<SingleDocumentLookupExecutor> buildIdLookupExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
        catalogResourceHandle);

}  // namespace mongo::exec::agg

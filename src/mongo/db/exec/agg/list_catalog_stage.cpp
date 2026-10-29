// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/exec/agg/list_catalog_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_list_catalog.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceListCatalogToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* listCatalogDS = dynamic_cast<DocumentSourceListCatalog*>(documentSource.get());

    tassert(10812303, "expected 'DocumentSourceListCatalog' type", listCatalogDS);

    return make_intrusive<exec::agg::ListCatalogStage>(listCatalogDS->kStageName,
                                                       listCatalogDS->getExpCtx());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(listCatalog,
                           DocumentSourceListCatalog::id,
                           documentSourceListCatalogToStageFn)

ListCatalogStage::ListCatalogStage(std::string_view stageName,
                                   const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : Stage(stageName, pExpCtx) {}

GetNextResult ListCatalogStage::doGetNext() {
    if (!_catalogDocs) {
        if (pExpCtx->getNamespaceString().isCollectionlessAggregateNS()) {
            _catalogDocs =
                pExpCtx->getMongoProcessInterface()->listCatalog(pExpCtx->getOperationContext());
        } else if (auto catalogDoc = pExpCtx->getMongoProcessInterface()->getCatalogEntry(
                       pExpCtx->getOperationContext(),
                       pExpCtx->getNamespaceString(),
                       pExpCtx->getUUID())) {
            _catalogDocs = {{std::move(*catalogDoc)}};
        } else {
            _catalogDocs.emplace();
        }
    }

    if (!_catalogDocs->empty()) {
        Document doc{_catalogDocs->front()};
        _catalogDocs->pop_front();
        return doc;
    }

    return GetNextResult::makeEOF();
}

}  // namespace exec::agg
}  // namespace mongo

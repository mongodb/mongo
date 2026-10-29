// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"


namespace mongo {
namespace exec {
namespace agg {

namespace {

stdx::unordered_map<DocumentSource::Id, DocumentSourceToStagesFn> stageBuildersMap;

}  // namespace

void registerDocumentSourceToStageFn(DocumentSource::Id dsid, DocumentSourceToStageFn fn) {
    registerDocumentSourceToStagesFn(
        dsid,
        [fn = std::move(fn)](const boost::intrusive_ptr<DocumentSource>& ds) -> StageExpansion {
            return {fn(ds)};
        });
}

void registerDocumentSourceToStagesFn(DocumentSource::Id dsid, DocumentSourceToStagesFn fn) {
    auto [_, inserted] = stageBuildersMap.insert({dsid, std::move(fn)});
    tassert(10395400, "duplicate DocumentSource to Stage mapping", inserted);
}

// Populate 'DocumentSource' to 'agg::Stage' mapping function registry after every 'DocumentSource'
// subclass got its unique 'Id' assigned.
MONGO_INITIALIZER_GROUP(BeginDocumentSourceStageRegistration, ("EndDocumentSourceIdAllocation"), ())
MONGO_INITIALIZER_GROUP(EndDocumentSourceStageRegistration,
                        ("BeginDocumentSourceStageRegistration"),
                        ())

StageExpansion buildStages(const boost::intrusive_ptr<DocumentSource>& ds) {
    auto it = stageBuildersMap.find(ds->getId());
    tassert(10395401,
            str::stream() << "missing 'DocumentSource' to 'agg::Stage' mapping function for "
                          << ds->getSourceName(),
            it != stageBuildersMap.end());
    auto expansion = (it->second)(ds);
    tassert(12634301,
            str::stream() << "stage expansion for " << ds->getSourceName()
                          << " must contain at least one stage",
            !expansion.empty());
    return expansion;
}

StagePtr buildStage(const boost::intrusive_ptr<DocumentSource>& ds) {
    auto expansion = buildStages(ds);
    tassert(10395402, "expected exactly one stage from 1:1 mapping", expansion.size() == 1);
    return std::move(expansion.front());
}

StagePtr buildStageAndStitch(const boost::intrusive_ptr<DocumentSource>& ds,
                             const StagePtr& sourceStage) {
    auto&& newStage = buildStage(ds);
    newStage->setSource(sourceStage.get());
    return newStage;
}

void stitchStage(Stage& stage, Stage* prior) {
    stage.setSource(prior);
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo

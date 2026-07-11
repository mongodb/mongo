// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/document_source_extension_for_query_shape.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/extension/host_connector/adapter/query_shape_opts_adapter.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extensionExpandable, DocumentSourceExtensionForQueryShape::id);

StageConstraints DocumentSourceExtensionForQueryShape::constraints(
    PipelineSplitState pipeState) const {
    // Default constraints for extension stages.
    //
    // Only DocumentSourceExtensionOptimizable has access to MongoExtensionStaticProperties and
    // overrides constraints() accordingly. DocumentSourceExtensionForQueryShape is a pre-desugar
    // wrapper around an AggStageParseNode and therefore always uses these defaults.
    //
    // This is acceptable because the aggregate command calls validateCommon() twice:
    //   (1) pre-desugar, when Expandable stages are still present, and
    //   (2) post-desugar/optimization, when all extension stages have been replaced by their
    //       expanded children, whose own constraints() reflect the true placement/host semantics.
    //
    // As long as validateCommon() is run again after desugaring, these defaults should remain as
    // lenient as possible to avoid prematurely rejecting a valid pipeline. If new callers begin
    // relying on constraints() before desugaring for correctness, we may need to surface
    // constraint metadata on the ParseNode or delay constraint checks until after desugar.
    //
    // TODO SERVER-117260 Change FacetRequirement.
    auto constraints = StageConstraints(StreamType::kStreaming,
                                        PositionRequirement::kNone,
                                        HostTypeRequirement::kNone,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kAllowed,
                                        UnionRequirement::kAllowed,
                                        ChangeStreamRequirement::kDenylist);
    constraints.canRunOnTimeseries = false;

    return constraints;
}

Value DocumentSourceExtensionForQueryShape::serialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isKeepingLiteralsUnchanged() && !opts.transformIdentifiers) {
        // Round-trippable serialization: emit the original user-provided stage so that callers
        // re-serializing this pipeline (e.g. a hybrid search desugarer embedding an input pipeline
        // in the $unionWith it constructs) produce BSON that re-parses to the same stage. Every
        // construction path populates _rawStage; an empty one means this wrapper was built in a
        // state that cannot round-trip, which should never reach literal-preserving serialization.
        tassert(11766200,
                "pre-desugar extension wrapper has no original BSON to round-trip",
                !_rawStage.isEmpty());
        return Value(Document{_rawStage});
    }
    // TODO SERVER-129346 Ideally we should tassert here that literals are being changed, but we
    // cannot because hybrid search desugars when running against query stats. Restore the tassert
    // when hybrid search can compute query shape without desugaring.
    host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};
    return Value(_parseNode->getQueryShape(adapter));
}

DocumentSource::Id DocumentSourceExtensionForQueryShape::getId() const {
    return id;
}

}  // namespace mongo::extension::host

namespace mongo {
void visitExtensionStage(DocsNeededBoundsContext* ctx,
                         const extension::host::DocumentSourceExtensionForQueryShape& source) {
    // extractDocsNeededBounds() runs after desugaring; a pre-desugar stage should never reach here.
    tasserted(11628000,
              "DocsNeededBounds visitor should not encounter a pre-desugar "
              "DocumentSourceExtensionForQueryShape");
}
}  // namespace mongo

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

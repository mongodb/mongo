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
    // TODO SERVER-117259 Change LookupRequirement.
    // TODO SERVER-117260 Change FacetRequirement.
    auto constraints = StageConstraints(StreamType::kStreaming,
                                        PositionRequirement::kNone,
                                        HostTypeRequirement::kNone,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
                                        UnionRequirement::kAllowed,
                                        ChangeStreamRequirement::kDenylist);
    constraints.canRunOnTimeseries = false;

    return constraints;
}

Value DocumentSourceExtensionForQueryShape::serialize(const SerializationOptions& opts) const {
    tassert(10978000,
            "SerializationOptions should change literals while represented as a "
            "DocumentSourceExtensionForQueryShape",
            !opts.isKeepingLiteralsUnchanged());

    host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};
    return Value(_parseNode->getQueryShape(adapter));
}

DocumentSource::Id DocumentSourceExtensionForQueryShape::getId() const {
    return id;
}

}  // namespace mongo::extension::host

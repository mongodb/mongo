// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Wraps a LiteParsedPipeline together with the BSON that the pipeline's stages were parsed from.
 * Use this whenever the backing BSON is a temporary that would otherwise die before the parsed
 * stages do — most commonly, subpipelines constructed inside a parent stage's lite parser.
 *
 * '_ownedStages' is declared before '_pipeline'. C++ initializes members in declaration order,
 * so the owned BSON copies exist before any BSONElement inside '_pipeline' references them.
 */
class OwnedLiteParsedPipeline {
public:
    OwnedLiteParsedPipeline(const NamespaceString& nss,
                            const std::vector<BSONObj>& pipelineStages,
                            const LiteParserOptions& options = {});

    // Moves already-parsed stages directly, without reparsing. Use this instead of the BSON-vector
    // constructor when the caller already holds StageSpecs (e.g. a desugarer re-assembling a
    // subpipeline) — reparsing loses information for stages whose original BSON is only a
    // placeholder (e.g. AST-only extension sub-stages), which can't round-trip through BSON.
    OwnedLiteParsedPipeline(NamespaceString nss, StageSpecs stages);

    OwnedLiteParsedPipeline(OwnedLiteParsedPipeline&&) noexcept = default;
    OwnedLiteParsedPipeline& operator=(OwnedLiteParsedPipeline&&) noexcept = default;

    OwnedLiteParsedPipeline(const OwnedLiteParsedPipeline& other);
    // Copy-assignment is not provided; construct a new OwnedLiteParsedPipeline explicitly.
    OwnedLiteParsedPipeline& operator=(const OwnedLiteParsedPipeline&) = delete;

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    LiteParsedPipeline& pipeline() {
        return _pipeline;
    }

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    const LiteParsedPipeline& pipeline() const {
        return _pipeline;
    }

    LiteParsedPipeline* operator->() {
        return &_pipeline;
    }

    const LiteParsedPipeline* operator->() const {
        return &_pipeline;
    }

    LiteParsedPipeline& operator*() {
        return _pipeline;
    }

    const LiteParsedPipeline& operator*() const {
        return _pipeline;
    }

    // The view NSS this pipeline was materialized from, if any. Set only for sub-pipelines created
    // by materializeViewSubpipeline() (e.g. the view pipeline materialized into a $graphLookup
    // stage), where the pipeline is parsed under the backing-collection NSS rather than the view's.
    // The current consumer is resolveInvolvedNamespacesImpl's cycle detection, which needs the view
    // NSS because the backing-collection NSS stored in _originalParseNss is not a view.
    const boost::optional<NamespaceString>& getViewNss() const {
        return _viewNss;
    }

    void setViewNss(NamespaceString nss) {
        _viewNss = std::move(nss);
    }

private:
    static std::vector<BSONObj> _makeStagesOwned(const std::vector<BSONObj>& pipelineStages);

    // Must be declared before '_pipeline': initializer-list order follows declaration order,
    // so the owned copies must exist before '_pipeline' parses them.
    std::vector<BSONObj> _ownedStages;
    LiteParsedPipeline _pipeline;
    boost::optional<NamespaceString> _viewNss;
};

}  // namespace mongo

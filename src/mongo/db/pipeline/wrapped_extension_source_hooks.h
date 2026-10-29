// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <set>
#include <string>

namespace mongo {

namespace rule_based_rewrites::pipeline {
class PipelineRewriteContext;
}  // namespace rule_based_rewrites::pipeline

/**
 * Interface for an extension source stage held as a member of another stage (e.g. the source stage
 * wrapped by $_internalDocumentResultsAndMetadata) rather than appearing as a top-level pipeline
 * element. Because the rule-based rewriter can't visit a wrapped stage directly, the wrapper drives
 * these interactions on behalf of its wrapped extension source stage.
 *
 * This interface exists to support extensions. It lives in db/pipeline (rather than under
 * extension/host) because the extension/host library already depends on db/pipeline and referencing
 * it directly would create a dependency cycle.
 */
class WrappedExtensionSourceHooks {
public:
    virtual ~WrappedExtensionSourceHooks() = default;

    virtual void skipMetadataStream() = 0;

    virtual void applyPipelineSuffixDependencies(const DepsTracker& deps,
                                                 const std::set<std::string>& builtinVarRefs) = 0;

    virtual void dispatchInPlaceRules(
        rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) const = 0;
};

}  // namespace mongo

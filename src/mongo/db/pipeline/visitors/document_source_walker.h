// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A walker over a DocumentSource pipeline. See the DocumentSourceVisitorRegistry header for details
 * about why this walker does not use the typical "visitor" interface.
 */
class DocumentSourceWalker final {
public:
    DocumentSourceWalker(const DocumentSourceVisitorRegistry& registry,
                         DocumentSourceVisitorContextBase* ctx)
        : _registry(registry), _visitorCtx(ctx) {}

    /**
     * Perform an pre-order traversal of the top-level document sources in the given pipeline (i.e.
     * does not walk $lookup/$unionWith subpipelines).
     */
    void walk(const Pipeline& pipeline);
    void walk(const DocumentSourceContainer& sources);

    /**
     * Same as walk(), but traverses the pipeline in reverse (back-to-front).
     */
    void reverseWalk(const Pipeline& pipeline);
    void reverseWalk(const DocumentSourceContainer& sources);
    void reverseWalk(const ConstDocumentSourceContainer& sources);

private:
    const DocumentSourceVisitorRegistry& _registry;
    DocumentSourceVisitorContextBase* _visitorCtx;
};

}  // namespace mongo

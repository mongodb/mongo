// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"

#include "mongo/db/pipeline/visitors/document_source_visitor_registry_mongos.h"

namespace mongo {
template <typename T>
void visit(DocsNeededBoundsContext* ctx, const T&) {
    ctx->applyUnknownStage();
}

const ServiceContext::ConstructorActionRegisterer DocsNeededBoundsRegisterer{
    "DocsNeededBoundsRegistererMongos", [](ServiceContext* service) {
        registerMongosVisitor<DocsNeededBoundsContext>(service);
    }};
}  // namespace mongo

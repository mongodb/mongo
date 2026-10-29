// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"

#include "mongo/db/pipeline/visitors/document_source_visitor_registry_mongod.h"

namespace mongo {
template <typename T>
void visit(DocsNeededBoundsContext* ctx, const T&) {
    ctx->applyUnknownStage();
}

const ServiceContext::ConstructorActionRegisterer DocsNeededBoundsRegisterer{
    "DocsNeededBoundsRegistererShardingRuntimeD", [](ServiceContext* service) {
        registerShardingRuntimeDVisitor<DocsNeededBoundsContext>(service);
    }};
}  // namespace mongo

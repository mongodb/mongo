// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Register 'visit()' functions for all mongos DocumentSources for the visitor specified as the
 * template parameter in the DocumentSource visitor regsitry in the given ServiceContext. Using this
 * function helps provide compile-time safety that ensures visitor implementors have provided an
 * implementation for all DocumentSoures. This function is intended to be used in the following
 * manner:
 *
 * void visit(FooVisitorCtx* ctx, const DocumentSourceMergeCursors& match) { ... }
 *
 * const ServiceContext::ConstructorActionRegisterer fooRegisterer{
 *   "FooRegisterer", [](ServiceContext* service) {
 *       registerMongosVisitor<FooVisitorCtx>(service);
 *   }};
 */
template <typename T>
void registerMongosVisitor(ServiceContext* service) {
    auto& registry = getDocumentSourceVisitorRegistry(service);
    registerVisitFuncs<T, DocumentSourceMergeCursors>(&registry);
}

}  // namespace mongo

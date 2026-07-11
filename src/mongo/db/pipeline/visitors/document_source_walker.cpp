// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/visitors/document_source_walker.h"

#include <list>


namespace mongo {

void DocumentSourceWalker::walk(const DocumentSourceContainer& sources) {
    for (auto&& ds : sources) {
        // Perform double-dispatch based on the visitor context and document source types by
        // consulting the registry.
        auto func = _registry.getConstVisitorFunc(*_visitorCtx, *ds);
        // Invoke the function pointer returned from the registry.
        func(_visitorCtx, *ds);
    }
}

namespace {
// Shared implementation for both container overloads. **reverseItr yields const DocumentSource&
// for both intrusive_ptr<DocumentSource> and intrusive_ptr<const DocumentSource>, so the
// registry dispatch is identical in both cases.
template <typename Container>
void reverseWalkImpl(const DocumentSourceVisitorRegistry& registry,
                     DocumentSourceVisitorContextBase* visitorCtx,
                     const Container& sources) {
    auto reverseItr = sources.rbegin();
    while (reverseItr != sources.rend()) {
        auto func = registry.getConstVisitorFunc(*visitorCtx, **reverseItr);
        func(visitorCtx, **reverseItr);
        reverseItr++;
    }
}
}  // namespace

void DocumentSourceWalker::reverseWalk(const DocumentSourceContainer& sources) {
    reverseWalkImpl(_registry, _visitorCtx, sources);
}

void DocumentSourceWalker::reverseWalk(const ConstDocumentSourceContainer& sources) {
    reverseWalkImpl(_registry, _visitorCtx, sources);
}

void DocumentSourceWalker::walk(const Pipeline& pipeline) {
    walk(pipeline.getSources());
}

void DocumentSourceWalker::reverseWalk(const Pipeline& pipeline) {
    reverseWalk(pipeline.getSources());
}
}  // namespace mongo

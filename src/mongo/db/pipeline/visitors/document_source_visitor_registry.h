/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/document_source.h"

#include <type_traits>
#include <unordered_map>

#include <fmt/format.h>

namespace mongo {

/**
 * Base class for all DocumentSource visitor contexts.
 *
 * This object enables clients to write visitors over DocumentSource pipelines. Typically, we'd
 * achieve this by defining a 'Visitor' abstract base class with virtual 'visit()' functions for
 * every DocumentSource. However, DocumentSources are special in that they can be dynamically
 * included depending on which libraries are linked into the binary. For example, there are
 * DocumentSources defined in the enterprise module that are not included in the community build of
 * the server. This means that we cannot have a class which implements a visitor for all document
 * sources.
 *
 * We solve this problem by having visitor implementations write 'visit()' functions as free
 * functions which take a DocumentSourceVisitorContextBase* as a parameter. Any state that the
 * visitor needs should be included in an object derived from this base. All the 'visit()' functions
 * should be added to the DocumentSourceVisitorRegistry during process initialization, which allows
 * the set of registered functions to be dynamic.
 *
 * WARNING: This approach sacrifices the compile-time safety of a traditional visitor in exchange
 * for the dynamic set of 'visit()' functions. Clients of this API must take care to ensure that all
 * necessary 'visit()' implementations have been provided. Failure to do so will result in the
 * DocumentSourceWalker throwing when encountering an unregistered DocumentSource.
 */
class DocumentSourceVisitorContextBase {
private:
    /**
     * This function is a no-op and should never be used. Its purpose is to have the compiler treat
     * DocumentSourceVisitorContextBase as a polymorphic class (as per the standard, which requires
     * at least one virtual function). This ensures that the typeid operator will return the
     * dynamic type of the object passed to it. This is needed when using the
     * DocumentSourceVisitorRegistry to dispatch a visitor function. See
     * https://en.cppreference.com/w/cpp/language/typeid for details.
     */
    virtual void noop() {}
};

struct DocumentSourceVisitorRegistryKey {
    template <typename H>
    friend H AbslHashValue(H h, const DocumentSourceVisitorRegistryKey& k) {
        return H::combine(std::move(h), k.visitorContextType, k.documentSourceType);
    }

    bool operator==(const DocumentSourceVisitorRegistryKey& other) const {
        return visitorContextType == other.visitorContextType &&
            documentSourceType == other.documentSourceType;
    }

    // We use std::type_index because std::type_info (the returned object from the typeid operator)
    // is not hashable.
    std::type_index visitorContextType;
    std::type_index documentSourceType;
};

using ConstVisitFunc = void (*)(DocumentSourceVisitorContextBase*, const DocumentSource&);

/**
 * A structure representing a virtual function table capable of resolving function calls based on
 * the runtime types of DocumentSourceVisitorContextBase and DocumentSource (typically solved by
 * double-dispatch). This is achieved by maintaining a map from (Visitor context type,
 * DocumentSource type) to function pointer.
 */
class DocumentSourceVisitorRegistry {
public:
    /**
     * Add a function to the registry keyed by the typeid of the given template args. Throws on
     * duplicate inserts.
     */
    template <typename VisitorCtx, typename DS>
    void registerVisitorFunc(ConstVisitFunc f) {
        static_assert(std::is_polymorphic_v<VisitorCtx>,
                      "Visitor context must be polymorphic to ensure typeid returns dynamic types");
        DocumentSourceVisitorRegistryKey key{std::type_index(typeid(VisitorCtx)),
                                             std::type_index(typeid(DS))};
        tassert(6202700,
                fmt::format("duplicate const document source visitor ({}, {}) registered",
                            key.visitorContextType.name(),
                            key.documentSourceType.name()),
                _constVisitorRegistry.emplace(key, f).second);
    }

    /**
     * Resolve a function based on the runtime types of the given a visitor context and document
     * source. Throws on missing entry.
     */
    ConstVisitFunc getConstVisitorFunc(DocumentSourceVisitorContextBase& visitorCtx,
                                       const DocumentSource& ds) const {
        DocumentSourceVisitorRegistryKey key{std::type_index(typeid(visitorCtx)),
                                             std::type_index(typeid(ds))};
        if (auto it = _constVisitorRegistry.find(key); it != _constVisitorRegistry.end()) {
            return it->second;
        }
        tasserted(6202701,
                  fmt::format("missing entry in const visitor registry: ({}, {})",
                              key.visitorContextType.name(),
                              key.documentSourceType.name()));
    }

private:
    stdx::unordered_map<DocumentSourceVisitorRegistryKey, ConstVisitFunc> _constVisitorRegistry;
};

/**
 * Helper template which allows users to define visit functions with type signatures using derived
 * types for VisitorContext and DocumentSource. Calls to registerVisitorFunc() may reference a
 * function pointer to this template which invokes the user's function. This avoids the boilerplate
 * of static casting the parameters to satisfy the type signature of the registry values.
 *
 * In other words, this function allows users to write:
 * void visit(ConcreteVisitor*, DocumentSourceMatch&);
 * instead of
 * void visit(DocumentSourceVisitorContextBase*, const DocumentSource&);
 * which would require them to cast the parameters.
 */
template <typename T, typename U>
void visit(DocumentSourceVisitorContextBase* ctx, const DocumentSource& ds) {
    // The visit() function below is defined by visitor implementers outside this file.
    visit(static_cast<T*>(ctx), static_cast<const U&>(ds));
}

// Base case of recursive template defined below.
template <typename VisitorCtx>
void registerVisitFuncs(DocumentSourceVisitorRegistry*) {}

/**
 * Convenience function for visitor implementers to register 'visit()' free functions into a
 * registry. Example usage:
 *
 * void visit(VisitorCtxFoo*, DocumentSourceBar&) { ... }
 * void visit(VisitorCtxFoo*, DocumentSourceBaz&) { ... }
 * DocumentSourceVisitorRegistry reg;
 * registerVisitFuncs<VisitorCtxFoo, DocumentSourceBar, DocumentSourceBaz>(&reg);
 */
template <typename VisitorCtx, typename D, typename... Ds>
void registerVisitFuncs(DocumentSourceVisitorRegistry* reg) {
    reg->registerVisitorFunc<VisitorCtx, D>(&visit<VisitorCtx, D>);
    // Invoke template recursively.
    registerVisitFuncs<VisitorCtx, Ds...>(reg);
}

// Declare visitor registry as a decoration on the service context.
inline const auto getDocumentSourceVisitorRegistry =
    ServiceContext::declareDecoration<DocumentSourceVisitorRegistry>();

}  // namespace mongo

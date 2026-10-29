// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>

namespace mongo::extension::host_connector {

/**
 * Boundary object representation of a ::MongoExtensionPipelineDependencies.
 *
 * This class wraps a host-side DepsTracker and exposes it through the C API vtable so that
 * extension code can query pipeline dependency information (metadata needs, whole-document needs)
 * without accessing host C++ types directly. The static VTABLE member points to static methods
 * which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionPipelineDependencies interface and layout as dictated by the public API.
 */
class PipelineDependenciesAdapter final : public ::MongoExtensionPipelineDependencies {
public:
    PipelineDependenciesAdapter(DepsTracker deps, std::set<std::string> variableRefs = {})
        : ::MongoExtensionPipelineDependencies{&VTABLE},
          _deps(std::move(deps)),
          _variableRefs(std::move(variableRefs)) {}

    ~PipelineDependenciesAdapter() = default;

    PipelineDependenciesAdapter(const PipelineDependenciesAdapter&) = delete;
    PipelineDependenciesAdapter& operator=(const PipelineDependenciesAdapter&) = delete;
    PipelineDependenciesAdapter(PipelineDependenciesAdapter&&) = delete;
    PipelineDependenciesAdapter& operator=(PipelineDependenciesAdapter&&) = delete;

    const DepsTracker& getImpl() const {
        return _deps;
    }

    const std::set<std::string>& getVariableRefs() const {
        return _variableRefs;
    }

private:
    static ::MongoExtensionStatus* _hostNeedsMetadata(
        const ::MongoExtensionPipelineDependencies* deps,
        ::MongoExtensionByteView name,
        bool* result) noexcept;
    static ::MongoExtensionStatus* _hostNeedsVariable(
        const ::MongoExtensionPipelineDependencies* deps,
        ::MongoExtensionByteView name,
        bool* result) noexcept;
    static ::MongoExtensionStatus* _hostNeedsWholeDocument(
        const ::MongoExtensionPipelineDependencies* deps, bool* result) noexcept;
    static ::MongoExtensionStatus* _hostGetNeededFields(
        const ::MongoExtensionPipelineDependencies* deps,
        ::MongoExtensionByteBuf** result) noexcept;

    static constexpr ::MongoExtensionPipelineDependenciesVTable VTABLE = {
        .needs_metadata = &_hostNeedsMetadata,
        .needs_variable = &_hostNeedsVariable,
        .needs_whole_document = &_hostNeedsWholeDocument,
        .get_needed_fields = &_hostGetNeededFields,
    };

    const DepsTracker _deps;
    const std::set<std::string> _variableRefs;
};

}  // namespace mongo::extension::host_connector

/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/extension/public/api.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"

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
    PipelineDependenciesAdapter(DepsTracker deps)
        : ::MongoExtensionPipelineDependencies{&VTABLE}, _deps(std::move(deps)) {}

    ~PipelineDependenciesAdapter() = default;

    PipelineDependenciesAdapter(const PipelineDependenciesAdapter&) = delete;
    PipelineDependenciesAdapter& operator=(const PipelineDependenciesAdapter&) = delete;
    PipelineDependenciesAdapter(PipelineDependenciesAdapter&&) = delete;
    PipelineDependenciesAdapter& operator=(PipelineDependenciesAdapter&&) = delete;

    const DepsTracker& getImpl() const {
        return _deps;
    }

private:
    static ::MongoExtensionStatus* _hostNeedsMetadata(
        const ::MongoExtensionPipelineDependencies* deps,
        ::MongoExtensionByteView name,
        bool* out) noexcept;
    static ::MongoExtensionStatus* _hostNeedsWholeDocument(
        const ::MongoExtensionPipelineDependencies* deps, bool* out) noexcept;

    static constexpr ::MongoExtensionPipelineDependenciesVTable VTABLE = {
        .needs_metadata = &_hostNeedsMetadata,
        .needs_whole_document = &_hostNeedsWholeDocument,
    };

    DepsTracker _deps;
};

}  // namespace mongo::extension::host_connector

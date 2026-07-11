// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

namespace mongo {

/**
 * Returns the dependencies for 'stage'.
 */
inline DepsTracker getDependencies(const DocumentSource& stage) {
    DepsTracker deps;
    stage.getDependencies(&deps);
    return deps;
}

}  // namespace mongo

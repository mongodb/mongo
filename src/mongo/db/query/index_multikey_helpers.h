// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Returns the positions of all path components in 'queryPath' that may be interpreted as array
 * indices by the query system. We obtain this list by finding all multikey path components that
 * have a numerical path component immediately after. Note that the 'queryPath' argument may be a
 * prefix of the full path used to generate 'multikeyPaths', and so we must avoid checking path
 * components beyond the end of 'queryPath'.
 */
inline std::vector<size_t> findArrayIndexPathComponents(const MultikeyComponents& multikeyPaths,
                                                        const FieldRef& queryPath) {
    std::vector<size_t> arrayIndices;
    for (auto i : multikeyPaths) {
        if (i < queryPath.numParts() - 1 && queryPath.isNumericPathComponentStrict(i + 1)) {
            arrayIndices.push_back(i + 1);
        }
    }
    return arrayIndices;
}

}  // namespace mongo

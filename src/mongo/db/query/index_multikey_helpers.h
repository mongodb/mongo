/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"

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

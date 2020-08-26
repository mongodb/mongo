/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/db/cst/c_node.h"

/**
 * Functions which perform additional validation beyond what a context free grammar can handle.
 * These return error messages which can be used to cause errors from inside the Bison parser.
 */
namespace mongo::c_node_validation {

enum class IsInclusion : bool { no, yes };

StatusWith<IsInclusion> validateProjectionAsInclusionOrExclusion(const CNode& projects);

Status validateNoConflictingPathsInProjectFields(const CNode& projects);

/**
 * Performs the following checks:
 * * Forbids empty path components.
 * * Path length is limited to the max allowable BSON depth.
 * * Forbids dollar characters.
 * * Forbids null bytes.
 */
Status validateAggregationPath(const std::vector<std::string>& pathComponents);

/**
 * Performs the following checks on the variable prefix:
 * * Forbides emptiness.
 * * Requires the first character to be a lowercase character or non-ascii.
 * * Requires all subsequent characters to be an alphanumeric, underscores or non-ascii.
 * Performs the following checks on the path components if any:
 * * Forbids empty path components.
 * * Path length is limited to the max allowable BSON depth.
 * * Forbids dollar characters.
 * * Forbids null bytes.
 */
Status validateVariableNameAndPathSuffix(const std::vector<std::string>& nameAndPathComponents);

enum class IsPositional : bool { no, yes };

/**
 * Determines if the projection is positional and performs the following checks:
 * * Forbids empty path components.
 * * Path length is limited to the max allowable BSON depth.
 * * Forbids dollar characters.
 * * Forbids null bytes.
 * 'pathComponents' is expected to contain at least one element.
 */
StatusWith<IsPositional> validateProjectionPathAsNormalOrPositional(
    const std::vector<std::string>& pathComponents);

}  // namespace mongo::c_node_validation

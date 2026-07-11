// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

namespace mongo {

class FieldRef;

namespace fieldchecker {

/**
 * Returns OK if all the below conditions on 'field' are valid:
 *   + Non-empty
 *   + Does not start or end with a '.'
 * Otherwise returns a code indicating cause of failure.
 */
Status isUpdatable(const FieldRef& field);

/**
 * Returns true iff 'field' is the position element (which is "$").
 */
bool isPositionalElement(std::string_view field);

/**
 * Returns true, the position 'pos' of the first $-sign if present in 'fieldRef', and
 * how many other $-signs were found in 'count'. Otherwise return false.
 *
 * Note:
 *   isPositional assumes that the field is updatable. Call isUpdatable() above to
 *   verify.
 */
bool isPositional(const FieldRef& fieldRef, size_t* pos, size_t* count = nullptr);

/**
 * Returns true iff 'field' is an array filter (matching the regular expression /\$\[.*\]/).
 */
bool isArrayFilterIdentifier(std::string_view field);

/**
 * Returns true if isArrayFilterIdentifier is true for any component in 'fieldRef' or returns false
   otherwise.
 */
bool hasArrayFilter(const FieldRef& fieldRef);

}  // namespace fieldchecker

}  // namespace mongo

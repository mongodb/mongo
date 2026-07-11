// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

namespace mongo {
namespace multikey_dotted_path_support {

/**
 * Expands arrays along the specified path and adds all elements to the 'elements' set.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements.
 *
 * This function fills 'arrayComponents' with the positions (starting at 0) of 'path' corresponding
 * to array values.
 *
 * Some examples:
 *
 *   Consider the document {a: [{b: 1}, {b: 2}]} and the path "a.b". The elements {b: 1} and {b: 2}
 *   would be added to the set. 'arrayComponents' would be set as std::set<size_t>{0U}.
 *
 *   Consider the document {a: [{b: [1, 2]}, {b: [2, 3]}]} and the path "a.b". The elements {b: 1},
 *   {b: 2}, and {b: 3} would be added to the set and 'arrayComponents' would be set as
 *   std::set<size_t>{0U, 1U} if 'expandArrayOnTrailingField' is true. The elements {b: [1, 2]} and
 *   {b: [2, 3]} would be added to the set and 'arrayComponents' would be set as
 *   std::set<size_t>{0U} if 'expandArrayOnTrailingField' is false.
 */
void extractAllElementsAlongPath(const BSONObj& obj,
                                 std::string_view path,
                                 BSONElementSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 MultikeyComponents* arrayComponents = nullptr);

void extractAllElementsAlongPath(const BSONObj& obj,
                                 std::string_view path,
                                 BSONElementMultiSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 MultikeyComponents* arrayComponents = nullptr);

/**
 * Legacy version of extractAllElementsAlongPath that uses pre-SERVER-76865 behavior.
 *
 * This version checks for literal field names with embedded dots BEFORE traversing
 * nested objects.
 *
 * For example, given document {"a.b": "x", "a": {"b": "y"}} and path "a.b":
 * - This function returns "x" (literal field "a.b")
 * - Current extractAllElementsAlongPath returns "y" (nested field a.b)
 *
 * Used only for validation to detect TEXT_INDEX_VERSION_3 indexes that need rebuilding.
 * Should not be used for any other purpose.
 */
void extractAllElementsAlongPathLegacy_forValidationOnly(
    const BSONObj& obj,
    std::string_view path,
    BSONElementSet& elements,
    bool expandArrayOnTrailingField = true,
    MultikeyComponents* arrayComponents = nullptr);

}  // namespace multikey_dotted_path_support
}  // namespace mongo

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"

#include <cstddef>

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
                                 StringData path,
                                 BSONElementSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 MultikeyComponents* arrayComponents = nullptr);

void extractAllElementsAlongPath(const BSONObj& obj,
                                 StringData path,
                                 BSONElementMultiSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 MultikeyComponents* arrayComponents = nullptr);

}  // namespace multikey_dotted_path_support
}  // namespace mongo

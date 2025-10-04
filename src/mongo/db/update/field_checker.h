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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

#include <cstddef>

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
bool isPositionalElement(StringData field);

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
bool isArrayFilterIdentifier(StringData field);

/**
 * Returns true if isArrayFilterIdentifier is true for any component in 'fieldRef' or returns false
   otherwise.
 */
bool hasArrayFilter(const FieldRef& fieldRef);

}  // namespace fieldchecker

}  // namespace mongo

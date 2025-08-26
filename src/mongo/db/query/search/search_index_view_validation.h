/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


namespace mongo {

class SearchQueryViewSpec;

namespace search_index_view_validation {

/**
 * Validates that the view's effective pipeline can be used with a search index. The restrictions
 * are as follows:
 *    - Only $addFields ($set) and $match can be used.
 *    - $addFields and cannot modify _id.
 *    - $match can only be used with $expr.
 *    - Variables $$NOW, $$CLUSTER_TIME, and $$USER_ROLES cannot be used.
 *    - Operators $rand and $function cannot be used.
 *    - Overriding the CURRENT variable with $let is not allowed.
 */
void validate(const SearchQueryViewSpec& view);

}  // namespace search_index_view_validation

}  // namespace mongo

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

#include "mongo/db/field_ref.h"
#include "mongo/stdx/unordered_set.h"

#include <set>
#include <string>
#include <vector>

namespace mongo {

struct IndexBounds;
struct IndexKeyEntry;
struct Interval;
struct MultikeyMetadataAccessStats;
class IndexCatalogEntry;
class OperationContext;

/**
 * Returns an exact set or super-set of the bounds required to fetch the multikey metadata keys
 * relevant to 'field'.
 */
std::vector<Interval> getMultikeyPathIndexIntervalsForField(FieldRef field);

/**
 * Returns the intersection of 'fields' and the set of multikey metadata paths stored in the
 * wildcard index. Statistics reporting index seeks and keys examined are written to 'stats'.
 */
std::set<FieldRef> getWildcardMultikeyPathSet(OperationContext* opCtx,
                                              const IndexCatalogEntry* entry,
                                              const stdx::unordered_set<std::string>& fieldSet,
                                              MultikeyMetadataAccessStats* stats);

/**
 * Returns the set of all paths for which the wildcard index has multikey metadata keys.
 * Statistics reporting index seeks and keys examined are written to 'stats'.
 */
std::set<FieldRef> getWildcardMultikeyPathSet(OperationContext* opCtx,
                                              const IndexCatalogEntry* entry,
                                              MultikeyMetadataAccessStats* stats);

}  // namespace mongo

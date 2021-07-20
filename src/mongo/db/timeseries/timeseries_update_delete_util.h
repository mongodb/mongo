/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"

namespace mongo::timeseries {

bool queryOnlyDependsOnMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const BSONObj& query,
                                 StringData metaField);

/**
 * Returns true if the given update modification only modifies the time-series collection's
 * metaField, false otherwise. Returns false on any document replacement.
 */
bool updateOnlyModifiesMetaField(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const write_ops::UpdateModification& updateMod,
                                 StringData metaField);

/**
 * Translates the given query to a query on the time-series collection's underlying buckets
 * collection. Creates and returns a translated query document where all occurrences of metaField in
 * query are replaced with the literal "meta". Requires that the given metaField is not empty.
 */
BSONObj translateQuery(const BSONObj& query, StringData metaField);

/**
 * Given a translated query and an update, creates and returns a translated update on the
 * time-series collection's underlying buckets collection where all occurrences of the given
 * metaField in updateMod are replaced with the literal "meta".
 */
write_ops::UpdateOpEntry translateUpdate(const BSONObj& translatedQuery,
                                         const write_ops::UpdateModification& updateMod,
                                         StringData metaField);

// TODO: SERVER-58394 Remove this method and combine its logic with
// timeseries::replaceTimeseriesQueryMetaFieldName().
/**
 * Recurses through the mutablebson element query and replaces any occurrences of the
 * metaField with "meta" accounting for queries that may be in dot notation. shouldReplaceFieldValue
 * is set for $expr queries when "$" + the metaField should be substituted for "$meta".
 */
void replaceTimeseriesQueryMetaFieldName(mutablebson::Element elem,
                                         const StringData& metaField,
                                         bool shouldReplaceFieldValue = false);

}  // namespace mongo::timeseries

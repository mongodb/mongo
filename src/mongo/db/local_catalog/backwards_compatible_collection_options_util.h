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

// TODO SERVER-92265 evaluate getting rid of this file

#include "mongo/base/string_data.h"
#include "mongo/db/repl/oplog_entry.h"

namespace mongo {

/**
 *
 * This utility is providing primitives to manage catalog parameters for which the actual value may
 * have been missing or incorrect in previous mongod [sub-]versions. It is only meant to fix catalog
 * issues in existing versions and must NOT be used for implementing new features.
 *
 * The functions under this namespace are offering an abstraction to work around the following
 * limitations:
 * - collMod command is strict (so can't simply add a parameter to previous mongod [sub-]versions)
 * - Collection options are strict (so can't simply add an option to previous mongod [sub-]versions)
 *
 */
namespace backwards_compatible_collection_options {

constexpr inline auto kTimeseriesBucketsMayHaveMixedSchemaData =
    "timeseriesBucketsMayHaveMixedSchemaData"_sd;
constexpr inline auto kTimeseriesBucketingParametersHaveChanged =
    "timeseriesBucketingParametersHaveChanged"_sd;

constexpr inline auto additionalCollModO2Field = "backwardsIncompatibleCollModParameters"_sd;

/**
 * Strips backwards incompatible fields from a collMod command and places them into a
 * different BSON object.
 *
 * Returns two BSON objects:
 * - A backwards compatible collMod oplog entry (not to generate crashes when applied by
 * incompatible mongod [sub-]versions).
 * - A field meant to be added to the `o2` sub-object (parsable by new mongod [sub-]versions).
 *
 * Example:
 *
 * - Original command:
 * {"collMod":"testdb.system.buckets.testcoll", "timeseriesBucketsMayHaveMixedSchemaData":true }
 *
 * - Expected oplog entry with `timeseriesBucketsMayHaveMixedSchemaData` backwards incompatible
 * collMod parameter.
 *
 * {
 *    "oplogEntry":{
 *        "op":"c",
 *        "ns":"testdb.$cmd",
 *        "ui":"UUID(""7302d025-cb9c-4a16-9222-0d5aeefbc039"")",
 *        "o":{
 *            "collMod":"system.buckets.testcoll"
 *         },
 *        "o2":{
 *            "collectionOptions_old":{
 *                "uuid": UUID("7302d025-cb9c-4a16-9222-0d5aeefbc039"),
 *                "Validator":{ "...REDACTED..." },
 *                "clusteredIndex":true,
 *                "timeseries":{
 *                    "timeField":"t",
 *                    “granularity":"seconds",
 *                    "bucketMaxSpanSeconds":3600
 *                }
 *            },
 *            "backwardsIncompatibleCollModParameters":{
 *                "timeseriesBucketsMayHaveMixedSchemaData":true
 *            }
 *        },
 *        "ts":Timestamp(1720003401,4),
 *        "t":1,
 *        "v":2,
 *        "wall":new Date(1720003401165)
 *   }
 * }
 *
 */
std::pair<BSONObj, BSONObj> getCollModCmdAndAdditionalO2Field(const BSONObj& collModCmd);

/**
 * Rebuilds a collMod command from an oplog entry.
 *
 * Returns a bson object:
 * - A collMod command inclusive of potential backwards incompatible fields present in the oplog
 * entry's `o2` sub-object.
 *
 * Example: collMod command parsed from the sample oplog entry documented above.
 *
 * {
 *     "collMod":"system.buckets.testcoll",
 *     "timeseriesBucketsMayHaveMixedSchemaData":true
 * }
 *
 */
BSONObj parseCollModCmdFromOplogEntry(const repl::OplogEntry& entry);

}  // namespace backwards_compatible_collection_options
}  // namespace mongo

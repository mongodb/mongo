// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// TODO SERVER-92265 evaluate getting rid of this file

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <string_view>

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
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] backwards_compatible_collection_options {

constexpr inline std::string_view kTimeseriesBucketsMayHaveMixedSchemaData =
    "timeseriesBucketsMayHaveMixedSchemaData";
constexpr inline std::string_view additionalCollModO2Field =
    "backwardsIncompatibleCollModParameters";

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

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// TODO SERVER-92265 evaluate getting rid of this util

#include "mongo/db/shard_role/shard_catalog/backwards_compatible_collection_options_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace backwards_compatible_collection_options {
namespace {
/**
 * Backwards incompatible catalog parameters for which the actual value may have been missing or
 * incorrect in previous mongod [sub-]versions.
 */
const StringDataSet kBackwardsCompatibleCollectionOptions{
    kTimeseriesBucketsMayHaveMixedSchemaData,
};
}  // namespace

std::pair<BSONObj, BSONObj> getCollModCmdAndAdditionalO2Field(const BSONObj& collModCmd) {
    const BSONObj collModCmdStrippedBackwardsIncompatibleParams =
        collModCmd.removeFields(kBackwardsCompatibleCollectionOptions);
    if (SimpleBSONObjComparator::kInstance.evaluate(collModCmdStrippedBackwardsIncompatibleParams ==
                                                    collModCmd)) {
        return {collModCmd, BSONObj()};
    }

    const BSONObj backwardsIncompatibleFields = [&]() {
        BSONObjBuilder bob;
        for (auto [fieldName, elem] : collModCmd) {
            if (kBackwardsCompatibleCollectionOptions.contains(fieldName)) {
                bob.append(elem);
            }
        }
        return bob.obj();
    }();

    return {collModCmdStrippedBackwardsIncompatibleParams, backwardsIncompatibleFields};
}

BSONObj parseCollModCmdFromOplogEntry(const repl::OplogEntry& entry) {
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't extract `collMod` command from non-collMod oplog entry: "
                          << entry.toBSONForLogging(),
            entry.getCommandType() == repl::OplogEntry::CommandType::kCollMod);

    if (!entry.getObject2()) {
        return entry.getObject();
    }

    BSONObj incompatibleFields = entry.getObject2()->getObjectField(additionalCollModO2Field);
    if (incompatibleFields.isEmpty()) {
        return entry.getObject();
    }

    // Only consider backwards incompatible fields supported in the current [sub-]version
    for (auto [fieldName, elem] : incompatibleFields) {
        if (!kBackwardsCompatibleCollectionOptions.contains(fieldName)) {
            incompatibleFields = incompatibleFields.removeField(fieldName);
        }
    }

    return entry.getObject().addFields(incompatibleFields);
}

}  // namespace backwards_compatible_collection_options
}  // namespace mongo

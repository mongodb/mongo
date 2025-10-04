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

// TODO SERVER-92265 evaluate getting rid of this util

#include "mongo/db/local_catalog/backwards_compatible_collection_options_util.h"

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
    kTimeseriesBucketingParametersHaveChanged,
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

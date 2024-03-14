/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/index_build_oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
StatusWith<IndexBuildOplogEntry> IndexBuildOplogEntry::parse(const repl::OplogEntry& entry) {
    // Example 'o' field which takes the same form for all three oplog entries.
    // {
    //     < "startIndexBuild" | "commitIndexBuild" | "abortIndexBuild" > : "coll",
    //     "indexBuildUUID" : <UUID>,
    //     "indexes" : [
    //         {
    //             "key" : {
    //                 "x" : 1
    //             },
    //             "name" : "x_1",
    //             ...
    //         },
    //         {
    //             "key" : {
    //                 "k" : 1
    //             },
    //             "name" : "k_1",
    //             ...
    //         }
    //     ],
    //     "cause" : <Object> // Only required for 'abortIndexBuild'.
    // }
    //
    //
    // Ensure the collection name is specified
    invariant(entry.getOpType() == repl::OpTypeEnum::kCommand);

    auto commandType = entry.getCommandType();
    invariant(commandType == repl::OplogEntry::CommandType::kStartIndexBuild ||
              commandType == repl::OplogEntry::CommandType::kCommitIndexBuild ||
              commandType == repl::OplogEntry::CommandType::kAbortIndexBuild);

    BSONObj obj = entry.getObject();
    BSONElement first = obj.firstElement();
    auto commandName = first.fieldNameStringData();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << commandName << " value must be a string",
            first.type() == mongo::String);

    auto buildUUIDElem = obj.getField("indexBuildUUID");
    if (buildUUIDElem.eoo()) {
        return {ErrorCodes::BadValue, str::stream() << "Missing required field 'indexBuildUUID'"};
    }

    auto swBuildUUID = UUID::parse(buildUUIDElem);
    if (!swBuildUUID.isOK()) {
        return swBuildUUID.getStatus().withContext("Error parsing 'indexBuildUUID'");
    }

    auto indexesElem = obj.getField("indexes");
    if (indexesElem.eoo()) {
        return {ErrorCodes::BadValue, str::stream() << "Missing required field 'indexes'"};
    }

    if (indexesElem.type() != Array) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field 'indexes' must be an array of index spec objects"};
    }

    std::vector<std::string> indexNames;
    std::vector<BSONObj> indexSpecs;
    for (auto& indexElem : indexesElem.Array()) {
        if (!indexElem.isABSONObj()) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Element of 'indexes' must be an object"};
        }
        std::string indexName;
        auto status = bsonExtractStringField(indexElem.Obj(), "name", &indexName);
        if (!status.isOK()) {
            return status.withContext("Error extracting 'name' from index spec");
        }
        indexNames.push_back(indexName);
        indexSpecs.push_back(indexElem.Obj().getOwned());
    }

    // Get the reason this index build was aborted on the primary.
    boost::optional<Status> cause;
    if (repl::OplogEntry::CommandType::kAbortIndexBuild == commandType) {
        auto causeElem = obj.getField("cause");
        if (causeElem.eoo()) {
            return {ErrorCodes::BadValue, "Missing required field 'cause'."};
        }
        if (causeElem.type() != Object) {
            return {ErrorCodes::BadValue, "Field 'cause' must be an object."};
        }
        auto causeStatusObj = causeElem.Obj();
        cause = getStatusFromCommandResult(causeStatusObj);
    }

    auto collUUID = entry.getUuid();
    invariant(collUUID, str::stream() << redact(entry.toBSONForLogging()));

    return IndexBuildOplogEntry{*collUUID,
                                commandType,
                                commandName.toString(),
                                swBuildUUID.getValue(),
                                std::move(indexNames),
                                std::move(indexSpecs),
                                cause,
                                entry.getOpTime()};
}
}  // namespace mongo

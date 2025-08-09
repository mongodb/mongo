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

#include "mongo/db/index_builds/index_build_oplog_entry.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
StatusWith<IndexBuildOplogEntry> IndexBuildOplogEntry::parse(const repl::OplogEntry& entry,
                                                             bool parseO2) {
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
    if (first.type() != BSONType::string) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << commandName << " value must be a string"};
    }

    auto buildUUIDElem = obj.getField("indexBuildUUID");
    if (buildUUIDElem.eoo()) {
        return {ErrorCodes::BadValue, "Missing required field 'indexBuildUUID'"};
    }

    auto swBuildUUID = UUID::parse(buildUUIDElem);
    if (!swBuildUUID.isOK()) {
        return swBuildUUID.getStatus().withContext("Error parsing 'indexBuildUUID'");
    }

    auto indexesElem = obj.getField("indexes");
    if (indexesElem.eoo()) {
        return {ErrorCodes::BadValue, "Missing required field 'indexes'"};
    }

    if (indexesElem.type() != BSONType::array) {
        return {ErrorCodes::BadValue, "Field 'indexes' must be an array of index spec objects"};
    }

    std::vector<IndexBuildInfo> indexesVec;
    for (auto& indexElem : indexesElem.Array()) {
        if (!indexElem.isABSONObj()) {
            return {ErrorCodes::BadValue, "Element of 'indexes' must be an object"};
        }
        std::string indexName;
        auto status = bsonExtractStringField(indexElem.Obj(), "name", &indexName);
        if (!status.isOK()) {
            return status.withContext("Error extracting 'name' from index spec");
        }
        indexesVec.emplace_back(indexElem.Obj().getOwned(), boost::none);
    }

    // Get the reason this index build was aborted on the primary.
    boost::optional<Status> cause;
    if (repl::OplogEntry::CommandType::kAbortIndexBuild == commandType) {
        auto causeElem = obj.getField("cause");
        if (causeElem.eoo()) {
            return {ErrorCodes::BadValue, "Missing required field 'cause'."};
        }
        if (causeElem.type() != BSONType::object) {
            return {ErrorCodes::BadValue, "Field 'cause' must be an object."};
        }
        auto causeStatusObj = causeElem.Obj();
        cause = getStatusFromCommandResult(causeStatusObj);
    }

    auto collUUID = entry.getUuid();
    invariant(collUUID, str::stream() << redact(entry.toBSONForLogging()));

    if (auto o2 = entry.getObject2(); o2 && parseO2) {
        auto indexesElem = o2->getField("indexes");
        if (indexesElem.eoo()) {
            return {ErrorCodes::BadValue, "Missing required field 'indexes'"};
        }
        if (indexesElem.type() != BSONType::array) {
            return {ErrorCodes::BadValue,
                    fmt::format("Field 'indexes' must be an array of objects, got '{}'",
                                indexesElem.toString())};
        }

        auto indexesElemVec = indexesElem.Array();
        if (indexesVec.size() != indexesElemVec.size()) {
            return {
                ErrorCodes::BadValue,
                fmt::format("'indexes' array sizes differ between o and o2 objects, got {} vs {}",
                            indexesVec.size(),
                            indexesElemVec.size())};
        }

        for (size_t i = 0; i < indexesElemVec.size(); ++i) {
            auto& indexElem = indexesElemVec[i];
            if (indexElem.type() != BSONType::object) {
                return {ErrorCodes::BadValue,
                        fmt::format("Element of 'indexes' must be an object, got '{}'",
                                    indexElem.toString())};
            }

            auto indexElemObj = indexElem.Obj();
            auto indexIdentElem = indexElemObj.getField("indexIdent");
            if (indexIdentElem.eoo()) {
                return {ErrorCodes::BadValue, "Missing required field 'indexIdent'"};
            }
            if (indexIdentElem.type() != BSONType::string) {
                return {ErrorCodes::BadValue,
                        fmt::format("'indexIdent' must be a string, got '{}'",
                                    indexIdentElem.toString())};
            }
            indexesVec[i].indexIdent = indexIdentElem.str();
        }
    }

    return IndexBuildOplogEntry{*collUUID,
                                commandType,
                                std::string{commandName},
                                swBuildUUID.getValue(),
                                std::move(indexesVec),
                                cause,
                                entry.getOpTime()};
}
}  // namespace mongo

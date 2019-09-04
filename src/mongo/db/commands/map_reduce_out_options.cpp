/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <string>
#include <utility>

#include "mongo/db/commands/map_reduce_out_options.h"

namespace mongo {

using namespace std::string_literals;

MapReduceOutOptions MapReduceOutOptions::parseFromBSON(const BSONElement& element) {
    if (element.type() == BSONType::String) {
        return MapReduceOutOptions(boost::none, element.str(), OutputType::Replace, false);
    } else if (element.type() == BSONType::Object) {
        const auto obj = element.embeddedObject();
        // The inline option is allowed alone.
        if (const auto inMemory = obj["inline"]) {
            uassert(ErrorCodes::BadValue, "'inline' must be specified alone", obj.nFields() == 1);
            uassert(
                ErrorCodes::BadValue, "'inline' takes only numeric '1'", inMemory.number() == 1.0);

            return MapReduceOutOptions(boost::none, "", OutputType::InMemory, false);
        }

        int allowedNFields = 3;

        const auto sharded = [&]() {
            if (const auto sharded = obj["sharded"]) {
                uassert(ErrorCodes::BadValue,
                        "sharded field value must be boolean",
                        sharded.type() == Bool);
                return sharded.boolean();
            } else {
                --allowedNFields;
                return false;
            }
        }();

        const auto databaseName = [&]() -> boost::optional<std::string> {
            if (const auto db = obj["db"]) {
                uassert(ErrorCodes::BadValue,
                        "db field value must be string",
                        db.type() == BSONType::String);
                return boost::make_optional(db.str());
            } else {
                --allowedNFields;
                return boost::none;
            }
        }();

        const auto [collectionName, outputType] = [&]() {
            auto stringOrError = [](auto&& element) {
                uassert(ErrorCodes::BadValue,
                        "'"s + element.fieldName() +
                            "' supports only string consisting of output collection name",
                        element.type() == BSONType::String);
                return element.str();
            };
            if (const auto replace = obj["replace"])
                return std::pair{stringOrError(replace), OutputType::Replace};
            else if (const auto merge = obj["merge"])
                return std::pair{stringOrError(merge), OutputType::Merge};
            else if (const auto reduce = obj["reduce"])
                return std::pair{stringOrError(reduce), OutputType::Reduce};
            else
                uasserted(ErrorCodes::BadValue, "'out' requires 'replace', 'merge' or 'reduce'");
        }();

        uassert(ErrorCodes::BadValue,
                "'out' supports only output type with collection name, optional 'sharded' and "
                "optional 'db'",
                obj.nFields() == allowedNFields);

        return MapReduceOutOptions(
            std::move(databaseName), std::move(collectionName), outputType, sharded);
    } else {
        uasserted(ErrorCodes::BadValue, "'out' must be either a string or an object");
    }
}

}  // namespace mongo

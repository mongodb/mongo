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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include <string>
#include <utility>

#include "mongo/db/commands/map_reduce_out_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"

namespace mongo {

using namespace std::string_literals;

// Used to occasionally log deprecation messages.
Rarely shardedDeprecationSampler;

MapReduceOutOptions MapReduceOutOptions::parseFromBSON(const BSONElement& element) {
    if (element.type() == BSONType::String) {
        return MapReduceOutOptions(boost::none, element.str(), OutputType::Replace, false);
    } else if (element.type() == BSONType::Object) {
        const auto obj = element.embeddedObject();
        // The inline option is allowed alone.
        if (const auto inMemory = obj["inline"]) {
            uassert(
                ErrorCodes::InvalidOptions, "'inline' must be specified alone", obj.nFields() == 1);
            uassert(
                ErrorCodes::BadValue, "'inline' takes only numeric '1'", inMemory.number() == 1.0);

            return MapReduceOutOptions(boost::none, "", OutputType::InMemory, false);
        }

        int allowedNFields = 4;

        const auto sharded = [&]() {
            if (const auto sharded = obj["sharded"]) {
                uassert(ErrorCodes::BadValue,
                        "sharded field value must be boolean",
                        sharded.type() == Bool);
                uassert(
                    ErrorCodes::BadValue, "sharded field value must be true", sharded.boolean());
                if (shardedDeprecationSampler.tick()) {
                    LOGV2_WARNING(23703, "The out.sharded option in MapReduce is deprecated");
                }
                return true;
            } else {
                --allowedNFields;
                return false;
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

        const auto databaseName =
            [&, &collectionName = collectionName]() -> boost::optional<std::string> {
            if (const auto db = obj["db"]) {
                uassert(ErrorCodes::BadValue,
                        "db field value must be string",
                        db.type() == BSONType::String);
                uassert(ErrorCodes::CommandNotSupported,
                        "cannot target internal database as output",
                        !(NamespaceString(db.valueStringData(), collectionName).isOnInternalDb()));
                return boost::make_optional(db.str());
            } else {
                --allowedNFields;
                return boost::none;
            }
        }();

        if (const auto nonAtomic = obj["nonAtomic"]) {
            uassert(ErrorCodes::BadValue,
                    "nonAtomic field value must be boolean",
                    nonAtomic.type() == Bool);
            uassert(
                ErrorCodes::BadValue, "nonAtomic field value must be true", nonAtomic.boolean());
        } else {
            --allowedNFields;
        }

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

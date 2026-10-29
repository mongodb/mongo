// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/commands/query_cmd/map_reduce_out_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using namespace std::string_literals;

// Used to occasionally log deprecation messages.
Rarely shardedDeprecationSampler;

MapReduceOutOptions MapReduceOutOptions::parseFromBSON(const BSONElement& element) {
    if (element.type() == BSONType::string) {
        return MapReduceOutOptions(boost::none, element.str(), OutputType::Replace, false);
    } else if (element.type() == BSONType::object) {
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
                        sharded.type() == BSONType::boolean);
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
                        element.type() == BSONType::string);
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
                        db.type() == BSONType::string);
                uassert(ErrorCodes::CommandNotSupported,
                        "cannot target internal database as output",
                        !(NamespaceStringUtil::deserialize(boost::none,
                                                           db.valueStringData(),
                                                           collectionName,
                                                           SerializationContext::stateDefault())
                              .isOnInternalDb()));
                return boost::make_optional(db.str());
            } else {
                --allowedNFields;
                return boost::none;
            }
        }();

        if (const auto nonAtomic = obj["nonAtomic"]) {
            uassert(ErrorCodes::BadValue,
                    "nonAtomic field value must be boolean",
                    nonAtomic.type() == BSONType::boolean);
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

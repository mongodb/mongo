/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;

/**
 * $log is a no-op stage that tests logging from the extension during query parse time.
 *
 * The stage definition expects an object with a field "numInfoLogLines" that has value within
 * [0,5]. The stage will never assert on unexpected input but instead will log lines depending on
 * the stage definition provided.
 */
DEFAULT_LOGICAL_AST_PARSE(Log);

class LogStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$log";
    static inline const std::string kNumInfoLogLinesField = "numInfoLogLines";

    LogStageDescriptor() : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        // Log an error log and short-circuit if the spec is empty or not an object.
        if (!stageBson.hasField(kStageName) || !stageBson.getField(kStageName).isABSONObj() ||
            stageBson.getField(kStageName).Obj().isEmpty()) {
            sdk::HostServicesHandle::getHostServices()->log(
                kStageName + " stage spec is empty or not an object.",
                11134000,
                mongo::extension::MongoExtensionLogSeverityEnum::kError);
            return std::make_unique<LogParseNode>();
        }

        mongo::BSONObj bsonSpec = stageBson.getField(kStageName).Obj();

        // Log a warning log if numInfoLogLines is not present, negative, or greater than 5, and
        // clamp it to the range [0,5].
        int numInfoLogLines = 0;
        if (!bsonSpec.hasElement(kNumInfoLogLinesField) ||
            !bsonSpec.getField(kNumInfoLogLinesField).isNumber()) {
            sdk::HostServicesHandle::getHostServices()->log(
                kStageName + " stage missing or invalid " + kNumInfoLogLinesField + " field.",
                11134001,
                mongo::extension::MongoExtensionLogSeverityEnum::kWarning);
        } else {
            numInfoLogLines = bsonSpec.getIntField(kNumInfoLogLinesField);

            if (numInfoLogLines < 0) {
                sdk::HostServicesHandle::getHostServices()->log(
                    kStageName + " stage must have non-negative value for " +
                        kNumInfoLogLinesField + ".",
                    11134002,
                    mongo::extension::MongoExtensionLogSeverityEnum::kWarning);
                numInfoLogLines = 0;
            } else if (numInfoLogLines > 5) {
                sdk::HostServicesHandle::getHostServices()->log(
                    kStageName + " stage will not print more than 5 log lines.",
                    11134003,
                    mongo::extension::MongoExtensionLogSeverityEnum::kWarning);
                numInfoLogLines = 5;
            }
        }

        // Log the requested number of info log lines.
        for (int i = 0; i < numInfoLogLines; i++) {
            // Uses the default severity of Info.
            sdk::HostServicesHandle::getHostServices()->log("Logging info line for " + kStageName,
                                                            11134004);
        }

        return std::make_unique<LogParseNode>();
    }
};

class LogExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<LogStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(LogExtension)
DEFINE_GET_EXTENSION()

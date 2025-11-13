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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;

/**
 * $log is a no-op stage that tests logging from the extension during query parse time.
 *
 * The stage definition expects an object with a field "numInfoLogLines" that has value within
 * [0,5]. The stage will never assert on unexpected input but instead will log lines depending on
 * the stage definition provided.
 */
DEFAULT_LOGICAL_AST_PARSE(Log, "$log");

class LogStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(LogStageName);
    static inline const std::string kNumInfoLogLinesField = "numInfoLogLines";
    static inline const std::string kAttributesField = "attrs";

    LogStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        // Log an error log and short-circuit if the spec is empty or not an object.
        if (!stageBson.hasField(kStageName) || !stageBson.getField(kStageName).isABSONObj() ||
            stageBson.getField(kStageName).Obj().isEmpty()) {
            sdk::sdk_log(kStageName + " stage spec is empty or not an object.",
                         11134000,
                         ::MongoExtensionLogSeverity::kError);
            return std::make_unique<LogParseNode>(stageBson);
        }

        mongo::BSONObj bsonSpec = stageBson.getField(kStageName).Obj();

        std::vector<mongo::extension::sdk::ExtensionLogAttribute> attrs;
        if (bsonSpec.hasElement(kAttributesField)) {
            auto attrsSpec = bsonSpec.getObjectField(kAttributesField);
            for (const auto& field : attrsSpec.getFieldNames<std::set<std::string>>()) {
                attrs.emplace_back(mongo::extension::sdk::ExtensionLogAttribute{
                    field, std::string(attrsSpec.getStringField(field))});
            }
        }

        // Log a warning log if numInfoLogLines is not present, negative, or greater than 5, and
        // clamp it to the range [0,5].
        int numInfoLogLines = 0;
        if (!bsonSpec.hasElement(kNumInfoLogLinesField) ||
            !bsonSpec.getField(kNumInfoLogLinesField).isNumber()) {
            sdk::sdk_log(kStageName + " stage missing or invalid " + kNumInfoLogLinesField +
                             " field.",
                         11134001,
                         ::MongoExtensionLogSeverity::kWarning,
                         attrs);
        } else {
            numInfoLogLines = bsonSpec.getIntField(kNumInfoLogLinesField);

            if (numInfoLogLines < 0) {
                sdk::sdk_log(kStageName + " stage must have non-negative value for " +
                                 kNumInfoLogLinesField + ".",
                             11134002,
                             ::MongoExtensionLogSeverity::kWarning,
                             attrs);
                numInfoLogLines = 0;
            } else if (numInfoLogLines > 5) {
                sdk::sdk_log(kStageName + " stage will not print more than 5 log lines.",
                             11134003,
                             ::MongoExtensionLogSeverity::kWarning,
                             attrs);
                numInfoLogLines = 5;
            }
        }

        // Log the requested number of info log lines.
        for (int i = 0; i < numInfoLogLines; i++) {
            // Uses the default severity of Info.
            sdk::sdk_log("Logging info line for " + kStageName, 11134004, attrs);
        }

        return std::make_unique<LogParseNode>(stageBson);
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

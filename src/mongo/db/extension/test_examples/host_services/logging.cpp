// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

/**
 * $log is a no-op stage that tests logging from the extension during query parse time.
 *
 * The stage definition expects an object with a field "numInfoLogLines" that has value within
 * [0,5]. The stage will never assert on unexpected input but instead will log lines depending on
 * the stage definition provided.
 */
class LogStageDescriptor
    : public sdk::TestStageDescriptor<"$log", sdk::shared_test_stages::TransformAggStageParseNode> {
public:
    static inline const std::string kNumInfoLogLinesField = "numInfoLogLines";
    static inline const std::string kAttributesField = "attrs";

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        mongo::BSONObj bsonSpec = sdk::validateStageDefinition(stageBson, kStageName);

        // Log an error log and short-circuit if the spec is empty or not an object.
        if (stageBson.getField(kStageName).Obj().isEmpty()) {
            sdk::sdk_log(kStageName + " stage spec is empty or not an object.",
                         11134000,
                         ::MongoExtensionLogSeverity::kError);
            return std::make_unique<sdk::shared_test_stages::TransformAggStageParseNode>(kStageName,
                                                                                         bsonSpec);
        }


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

        return std::make_unique<sdk::shared_test_stages::TransformAggStageParseNode>(kStageName,
                                                                                     bsonSpec);
    }
};

DEFAULT_EXTENSION(Log)
REGISTER_EXTENSION(LogExtension)
DEFINE_GET_EXTENSION()

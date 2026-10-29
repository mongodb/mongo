// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

/**
 * $debugLog is a no-op stage that tests debug logging from the extension during query parse time.
 *
 * The stage definition expects an object with a field "level" that has value within [1,5]. The
 * stage will never assert on unexpected input but instead will log lines depending on
 * the level provided and the server's log level.
 */
class DebugLogStageDescriptor
    : public sdk::TestStageDescriptor<"$debugLog",
                                      sdk::shared_test_stages::TransformAggStageParseNode> {
public:
    static inline const std::string kDebugLogLevelField = "level";
    static inline const std::string kAttributesField = "attrs";

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto bsonSpec = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(11134101,
                    "Failed to parse " + kStageName + ", expected non-empty object",
                    !bsonSpec.isEmpty());

        sdk_uassert(11134102,
                    kStageName + " stage missing or invalid " + kDebugLogLevelField + " field",
                    bsonSpec.hasElement(kDebugLogLevelField) &&
                        bsonSpec.getField(kDebugLogLevelField).isNumber());

        int level = bsonSpec.getIntField(kDebugLogLevelField);

        // This tests the functionality of the shouldLog host service.
        if (sdk::HostServicesAPI::getInstance()->getLogger()->shouldLog(
                ::MongoExtensionLogSeverity(level), ::MongoExtensionLogType::kDebug)) {
            sdk::sdk_log("Log level is enough", 11134101, ::MongoExtensionLogSeverity::kWarning);
        } else {
            sdk::sdk_log(
                "Log level is not enough", 11134102, ::MongoExtensionLogSeverity::kWarning);
        }

        std::vector<mongo::extension::sdk::ExtensionLogAttribute> attrs;
        if (bsonSpec.hasElement(kAttributesField)) {
            auto attrsSpec = bsonSpec.getObjectField(kAttributesField);
            for (const auto& field : attrsSpec.getFieldNames<std::set<std::string>>()) {
                attrs.emplace_back(mongo::extension::sdk::ExtensionLogAttribute{
                    field, std::string(attrsSpec.getStringField(field))});
            }
        }

        sdk::sdk_logDebug("Test log message", 11134100, level, attrs);

        return std::make_unique<sdk::shared_test_stages::TransformAggStageParseNode>(kStageName,
                                                                                     bsonSpec);
    }
};

DEFAULT_EXTENSION(DebugLog);
REGISTER_EXTENSION(DebugLogExtension)
DEFINE_GET_EXTENSION()

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
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;

/**
 * $debugLog is a no-op stage that tests debug logging from the extension during query parse time.
 *
 * The stage definition expects an object with a field "level" that has value within [1,5]. The
 * stage will never assert on unexpected input but instead will log lines depending on
 * the level provided and the server's log level.
 */
DEFAULT_LOGICAL_AST_PARSE(DebugLog, "$debugLog");

class DebugLogStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(DebugLogStageName);
    static inline const std::string kDebugLogLevelField = "level";

    DebugLogStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(11134101,
                    "Failed to parse " + kStageName + ", expected non-empty object",
                    !stageBson.getField(kStageName).Obj().isEmpty());

        mongo::BSONObj bsonSpec = stageBson.getField(kStageName).Obj();
        sdk_uassert(11134102,
                    kStageName + " stage missing or invalid " + kDebugLogLevelField + " field.",
                    bsonSpec.hasElement(kDebugLogLevelField) &&
                        bsonSpec.getField(kDebugLogLevelField).isNumber());

        int level = bsonSpec.getIntField(kDebugLogLevelField);

        // This tests the functionality of the shouldLog host service.
        if (sdk::HostServicesHandle::getHostServices()->shouldLog(
                ::MongoExtensionLogSeverity(level), ::MongoExtensionLogType::kDebug)) {
            sdk::HostServicesHandle::getHostServices()->log(
                "Log level is enough", 11134101, ::MongoExtensionLogSeverity::kWarning);
        } else {
            sdk::HostServicesHandle::getHostServices()->log(
                "Log level is not enough", 11134102, ::MongoExtensionLogSeverity::kWarning);
        }

        sdk::HostServicesHandle::getHostServices()->logDebug("Test log message", 11134100, level);

        return std::make_unique<DebugLogParseNode>(stageBson);
    }
};

class DebugLogExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<DebugLogStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(DebugLogExtension)
DEFINE_GET_EXTENSION()

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

#include "mongo/db/extension/host/load_stub_parsers.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/logv2/log.h"

#include <fstream>
#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {
const std::filesystem::path& getExtensionStubParserFile() {
    static const std::filesystem::path kExtensionStubParserPath = [] {
        constexpr auto kFileName = "aggregation_stage_fallback_parsers.json";
        if (getTestCommandsEnabled()) {
            return std::filesystem::current_path() /
                std::filesystem::path{"src/mongo/db/extension/test_examples"} / kFileName;
        }
        return ExtensionLoader::kExtensionConfigPath / kFileName;
    }();

    return kExtensionStubParserPath;
}

/**
 * The structure of the stub parser file looks like:
 * {
 *   "stubParsers": [
 *      {"stageName": "$foo", "message": "$foo is not available because..."},
 *      {"stageName": "$bar", "message": "$bar is not available because...", "featureFlag":
 * "<nameOfExistingFeatureFlag>"},
 *   ]
 * }
 */
static constexpr StringData kExtensionStubParserField = "stubParsers"_sd;
static constexpr StringData kExtensionStubParserStageNameField = "stageName"_sd;
static constexpr StringData kExtensionStubParserMessageField = "message"_sd;
static constexpr StringData kExtensionStubParserFeatureFlagField = "featureFlag"_sd;
}  // namespace

void registerStubParser(std::string stageName, std::string message, FeatureFlag* featureFlag) {
    LOGV2(10918509,
          "Registering fallback stub parser for extension stage",
          "stageName"_attr = stageName,
          "message"_attr = message);

    // Register a fallback parser that throws the appropriate error. Since this throws during
    // LiteParse, we never reach DocumentSource parsing and DocumentSource registration is not
    // needed.
    auto stubParser = [message = std::move(message)](
                          const NamespaceString&,
                          const BSONElement&,
                          const LiteParserOptions&) -> std::unique_ptr<LiteParsedDocumentSource> {
        uasserted(10918500, message);
    };

    LiteParsedDocumentSource::registerFallbackParser(std::move(stageName),
                                                     std::move(stubParser),
                                                     featureFlag,
                                                     AllowedWithApiStrict::kAlways,
                                                     AllowedWithClientType::kAny,
                                                     true /*isStub*/);
}

void registerUnloadedExtensionStubParsers() {
    const auto extensionStubParserFile = getExtensionStubParserFile();
    if (!std::filesystem::exists(extensionStubParserFile)) {
        LOGV2_DEBUG(10918501,
                    2,
                    "No stub parser file for unloaded extensions",
                    "filePath"_attr = extensionStubParserFile.string());
        return;
    }

    std::ifstream stubParserFile(extensionStubParserFile);

    if (!stubParserFile.is_open()) {
        LOGV2_WARNING_OPTIONS(10918502,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to open extension stub parser file",
                              "filePath"_attr = extensionStubParserFile.string());
        return;
    }

    const auto jsonStr = std::string{(std::istreambuf_iterator<char>(stubParserFile)),
                                     std::istreambuf_iterator<char>()};
    const auto& obj = fromjson(jsonStr);
    const auto& stubParserArrayElem = obj[kExtensionStubParserField];

    if (stubParserArrayElem.type() != BSONType::array) {
        LOGV2_WARNING_OPTIONS(10918503,
                              {logv2::LogTag::kStartupWarnings},
                              "Expected 'stubParsers' field to be an array",
                              "filePath"_attr = extensionStubParserFile.string());
        return;
    }

    for (const auto& elem : stubParserArrayElem.Array()) {
        if (elem.type() != BSONType::object) {
            LOGV2_WARNING_OPTIONS(10918504,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Expected stub parser entry to be an object",
                                  "stubEntry"_attr = elem.toString());
            continue;
        }

        const auto& stubObj = elem.Obj();
        const auto& stageNameElem = stubObj[kExtensionStubParserStageNameField];
        const auto& messageElem = stubObj[kExtensionStubParserMessageField];
        const auto& featureFlagElem = stubObj[kExtensionStubParserFeatureFlagField];

        bool stageNameIsString = stageNameElem.type() == BSONType::string;
        bool messageIsString = messageElem.type() == BSONType::string;
        bool featureFlagIsStringIfExists =
            featureFlagElem.type() == BSONType::eoo || featureFlagElem.type() == BSONType::string;
        if (!stageNameIsString || !messageIsString || !featureFlagIsStringIfExists) {
            LOGV2_WARNING_OPTIONS(10918505,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Stub parsers expect 'stageName', 'message', and optional "
                                  "'featureFlag' fields to be strings",
                                  "stubEntry"_attr = stubObj.toString());
            continue;
        }

        const auto stageName = stageNameElem.String();
        const auto message = messageElem.String();

        if (message.empty()) {
            LOGV2_WARNING_OPTIONS(10918507,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Stub entry's error message cannot be empty",
                                  "stubEntry"_attr = stubObj.toString());
            continue;
        }

        FeatureFlag* featureFlag = nullptr;
        if (featureFlagElem.type() == BSONType::string) {
            const auto featureFlagName = featureFlagElem.String();
            featureFlag = IncrementalRolloutFeatureFlag::findByName(featureFlagName);
            if (!featureFlag) {
                LOGV2_WARNING_OPTIONS(11395402,
                                      {logv2::LogTag::kStartupWarnings},
                                      "Stub parser references unknown feature flag",
                                      "stageName"_attr = stageName,
                                      "featureFlagName"_attr = featureFlagName);
            }
        }

        registerStubParser(std::move(stageName), std::move(message), featureFlag);
    }
}
}  // namespace mongo::extension::host

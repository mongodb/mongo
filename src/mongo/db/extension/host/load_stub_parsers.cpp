// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/load_stub_parsers.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"

#include <fstream>
#include <iostream>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {
using namespace std::literals::string_view_literals;
std::filesystem::path getExtensionStubParserDirectory() {
    if (getTestCommandsEnabled()) {
        return std::filesystem::current_path() /
            std::filesystem::path{"src/mongo/db/extension/test_examples"};
    }
    // In production, the stub parsers are expected to be found in the same directory as the
    // extension configuration files.
    return std::filesystem::path(serverGlobalParams.extensionsConfigPath);
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
static constexpr std::string_view kExtensionStubParserField = "stubParsers"sv;
static constexpr std::string_view kExtensionStubParserStageNameField = "stageName"sv;
static constexpr std::string_view kExtensionStubParserMessageField = "message"sv;
static constexpr std::string_view kExtensionStubParserFeatureFlagField = "featureFlag"sv;
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

    LiteParsedDocumentSource::registerFallbackParser(
        std::move(stageName),
        featureFlag,
        {.parser = std::move(stubParser),
         .fromExtension = true,
         .isStub = true,
         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
         .allowedWithClientType = AllowedWithClientType::kAny});
}

void registerUnloadedExtensionStubParsers() {
    const auto extensionStubParserDirectory = getExtensionStubParserDirectory();
    if (extensionStubParserDirectory.empty()) {
        LOGV2_DEBUG(
            12773201, 2, "No stub parser file for unloaded extension - no directory provided");
        return;
    }
    constexpr auto kFileName = "aggregation_stage_fallback_parsers.json";
    const auto extensionStubParserFile = extensionStubParserDirectory / kFileName;
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

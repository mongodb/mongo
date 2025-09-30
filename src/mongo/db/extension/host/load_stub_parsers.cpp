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
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/logv2/log.h"

#include <fstream>
#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {
const std::filesystem::path& getExtensionStubParserFile() {
    static const std::filesystem::path kExtensionStubParserPath = [] {
        constexpr auto kFileName = "aggregation_stage_stub_parsers.json";
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
 *      {"stageName": "$bar", "message": "$bar is not available because..."},
 *   ]
 * }
 */
static constexpr StringData kExtensionStubParserField = "stubParsers"_sd;
static constexpr StringData kExtensionStubParserStageNameField = "stageName"_sd;
static constexpr StringData kExtensionStubParserMessageField = "message"_sd;
}  // namespace


void registerStubParser(std::string stageName, std::string message) {
    LOGV2_DEBUG(10918509,
                2,
                "Registering stub parser for extension stage",
                "stageName"_attr = stageName,
                "message"_attr = message);
    LiteParsedDocumentSource::registerParser(stageName,
                                             LiteParsedDocumentSourceDefault::parse,
                                             AllowedWithApiStrict::kAlways,
                                             AllowedWithClientType::kAny);
    DocumentSource::registerParser(
        std::move(stageName),
        [message = std::move(message)](BSONElement elem,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx)
            -> std::list<boost::intrusive_ptr<DocumentSource>> { uasserted(10918500, message); },
        nullptr /* featureFlag */,
        // Since skipIfExists is true, if the stage is already registered, this registration will be
        // silently skipped.
        true /* skipIfExists */);
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
        LOGV2_ERROR(10918502,
                    "Failed to open extension stub parser file",
                    "filePath"_attr = extensionStubParserFile.string());
        return;
    }

    const auto jsonStr = std::string{(std::istreambuf_iterator<char>(stubParserFile)),
                                     std::istreambuf_iterator<char>()};
    const auto& obj = fromjson(jsonStr);
    const auto& stubParserArrayElem = obj[kExtensionStubParserField];

    if (stubParserArrayElem.type() != BSONType::array) {
        LOGV2_ERROR(10918503,
                    "Expected stub parser field to be an array",
                    "fieldName"_attr = kExtensionStubParserField,
                    "filePath"_attr = extensionStubParserFile.string());
        return;
    }

    for (const auto& elem : stubParserArrayElem.Array()) {
        if (elem.type() != BSONType::object) {
            LOGV2_ERROR(
                10918504, "Expected stub parser entry to be an object", "stubEntry"_attr = elem);
            return;
        }

        const auto& stubObj = elem.Obj();
        const auto& stageNameElem = stubObj[kExtensionStubParserStageNameField];
        const auto& messageElem = stubObj[kExtensionStubParserMessageField];

        if (stageNameElem.type() != BSONType::string || messageElem.type() != BSONType::string) {
            LOGV2_ERROR(10918505,
                        "Stub parsers expect both 'stageName' and 'message' fields to be strings",
                        "stubEntry"_attr = stubObj.toString());
            continue;
        }

        const auto stageName = stageNameElem.String();
        const auto message = messageElem.String();

        if (message.empty()) {
            LOGV2_ERROR(10918507,
                        "Stub entry's error message cannot be empty",
                        "stubEntry"_attr = stubObj.toString());
            continue;
        }

        registerStubParser(std::move(stageName), std::move(message));
    }
}
}  // namespace mongo::extension::host

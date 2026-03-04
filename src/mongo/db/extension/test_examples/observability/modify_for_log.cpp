/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * $modifyForLog is a test stage that demonstrates various modifications in toBsonForLog().
 * It can truncate large arrays, summarize objects, and add metadata.
 *
 * Stage syntax:
 * {
 *   $modifyForLog: {
 *     "largeArray": [1, 2, 3, ..., 1000],
 *     "nestedObject": { ... },
 *     "normalField": "value"
 *   }
 * }
 *
 * In logs, it will:
 * - Truncate arrays longer than 5 elements
 * - Add a summary for nested objects
 * - Add metadata about modifications
 */
class ModifyForLogParseNode
    : public sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode> {
public:
    ModifyForLogParseNode(const BSONObj& input) : ModifyForLogParseNode("$modifyForLog", input) {}

    ModifyForLogParseNode(std::string_view stageName, const BSONObj& input)
        : sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode>(stageName, input) {}

    BSONObj toBsonForLog() const override {
        BSONObjBuilder modifiedBuilder;
        bool wasModified = false;

        for (const auto& elem : _arguments) {
            auto fieldName = elem.fieldNameStringData();

            if (elem.type() == BSONType::array) {
                // Truncate arrays with more than 5 elements down to 3.
                std::vector<BSONElement> arrayElements = elem.Array();
                if (arrayElements.size() > 5) {
                    BSONArrayBuilder truncatedArray;
                    for (size_t i = 0; i < 3; i++) {
                        truncatedArray.append(arrayElements[i]);
                    }
                    modifiedBuilder.append(fieldName, truncatedArray.arr());
                    wasModified = true;
                } else {
                    modifiedBuilder.append(elem);
                }
            } else if (elem.type() == BSONType::object) {
                // Summarize nested objects.
                BSONObj nestedObj = elem.Obj();
                if (nestedObj.nFields() > 5) {
                    modifiedBuilder.append(
                        fieldName,
                        BSON("summary"
                             << "object with " + std::to_string(nestedObj.nFields()) + " fields"));
                    wasModified = true;
                } else {
                    modifiedBuilder.append(elem);
                }
            } else {
                modifiedBuilder.append(elem);
            }
        }

        BSONObjBuilder resultBuilder;
        resultBuilder.append("spec", modifiedBuilder.obj());
        if (wasModified) {
            resultBuilder.append("logNote", "Some fields were modified for logging");
        }

        return BSON(_name << resultBuilder.obj());
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<ModifyForLogParseNode>(getName(), _arguments);
    }
};

using ModifyForLogStageDescriptor =
    sdk::TestStageDescriptor<"$modifyForLog", ModifyForLogParseNode>;

class ModifyForLogExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ModifyForLogStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(ModifyForLogExtension)
DEFINE_GET_EXTENSION()

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

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

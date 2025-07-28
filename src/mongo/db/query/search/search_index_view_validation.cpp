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

#include "mongo/db/query/search/search_index_view_validation.h"

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace search_index_view_validation {

static constexpr StringData errorPrefix =
    "Cannot perform a search operation as the view definition is incompatible with Atlas Search: "_sd;

namespace {

// Sets of banned variables and operators for view definitions with a search index.
static const StringDataSet bannedVariables = {
    Variables::kNowName, Variables::kClusterTimeName, Variables::kUserRolesName};
static const StringDataSet bannedOperators = {"$rand", ExpressionFunction::kExpressionName};

void matchValidator(const BSONObj& stage) {
    uassert(10623001,
            str::stream() << errorPrefix << "$match stage can only contain $expr",
            stage.hasField("$expr") && stage.nFields() == 1);
}

void modificationValidator(const BSONObj& stage) {
    uassert(10623002,
            str::stream() << errorPrefix << "Modifying _id field is not allowed",
            !stage.hasField("_id"));
}

void validateHelper(const BSONObj& obj, std::function<void(const BSONElement&)> documentValidator) {
    // Recursively call validateHelper through each sub-document or array. Then, call the document
    // validator.
    for (const auto& field : obj) {
        if (field.isABSONObj()) {
            validateHelper(field.embeddedObject(), documentValidator);
        }
        documentValidator(field);
    }
}

void validateOperators(StringData field) {
    uassert(10623003,
            str::stream() << errorPrefix << field << " is not allowed",
            !bannedOperators.contains(field));
}

void validateVariables(StringData value) {
    if (value.empty()) {
        return;
    }

    // Check for any of the banned variables in the value.
    bool containsBannedVariable =
        std::any_of(bannedVariables.begin(), bannedVariables.end(), [&value](StringData variable) {
            return value.find("$$" + variable) != std::string::npos;
        });

    uassert(10623004,
            str::stream()
                << errorPrefix
                << "Using variables like $$NOW, $$CLUSTER_TIME, or $$USER_ROLES is not allowed",
            !containsBannedVariable);
};

void checkForAggregationVariablesOverride(const BSONElement& elem) {
    // Check for $let.vars.CURRENT.
    if (elem.fieldNameStringData() == "$let" && elem.isABSONObj()) {
        const auto& varsObject = elem.embeddedObject().getObjectField("vars");
        if (!varsObject.isEmpty()) {
            uassert(10623005,
                    str::stream() << errorPrefix
                                  << "Overriding the CURRENT variable is not allowed",
                    !varsObject.hasField("CURRENT"));
        }
    }
}

// Map from stage name to stage-specific validator.
static const StringDataMap<std::function<void(const BSONObj&)>> stageValidators = {
    {DocumentSourceAddFields::kStageName, modificationValidator},
    {DocumentSourceAddFields::kAliasNameSet, modificationValidator},
    {DocumentSourceMatch::kStageName, matchValidator}};

// Returns the stage-specific validator.
std::function<void(const BSONObj&)> getStageValidator(StringData stageName) {
    auto validator = stageValidators.find(stageName);

    uassert(10623000,
            str::stream() << errorPrefix << stageName << " is not allowed",
            validator != stageValidators.end());

    return validator->second;
}

}  // namespace

void validate(const SearchQueryViewSpec& view) {
    for (const auto& stage : view.getEffectivePipeline()) {
        // Get and call the stage-specific validator.
        auto stageValidator = getStageValidator(stage.firstElementFieldNameStringData());
        stageValidator(stage.firstElement().Obj());

        // Recursively call the general validator that applies to all stages.
        validateHelper(stage, [](const BSONElement& elem) {
            validateOperators(elem.fieldNameStringData());
            validateVariables(elem.valueStringDataSafe());
            checkForAggregationVariablesOverride(elem);
        });
    }
}

}  // namespace search_index_view_validation

}  // namespace mongo

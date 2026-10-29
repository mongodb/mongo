// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/search_index_view_validation.h"

#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <string_view>

namespace mongo {

namespace search_index_view_validation {
using namespace std::literals::string_view_literals;

static constexpr std::string_view errorPrefix =
    "Cannot perform a search operation as the view definition is incompatible with Atlas Search: "sv;

namespace {

// Sets of banned variables and operators for view definitions with a search index.
static const StringDataSet bannedVariables = {
    Variables::kNowName, Variables::kClusterTimeName, Variables::kUserRolesName};
static const StringDataSet bannedOperators = {"$rand", ExpressionFunction::kExpressionName};

void matchValidator(const BSONObj& stage) {
    uassert(10623001,
            str::stream() << errorPrefix << "$match stage can only contain $expr",
            stage.isEmpty() || (stage.hasField("$expr") && stage.nFields() == 1));
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

void validateOperators(std::string_view field) {
    uassert(10623003,
            str::stream() << errorPrefix << field << " is not allowed",
            !bannedOperators.contains(field));
}

void validateVariables(std::string_view value) {
    if (value.empty()) {
        return;
    }

    // Check for any of the banned variables in the value.
    bool containsBannedVariable = std::any_of(
        bannedVariables.begin(), bannedVariables.end(), [&value](std::string_view variable) {
            return value.find("$$" + std::string{variable}) != std::string::npos;
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
std::function<void(const BSONObj&)> getStageValidator(std::string_view stageName) {
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

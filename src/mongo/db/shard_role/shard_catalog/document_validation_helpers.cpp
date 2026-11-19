// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/document_validation_helpers.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

Status checkValidationOptionsCanBeUsed(const CollectionOptions& opts,
                                       boost::optional<ValidationLevelEnum> newLevel,
                                       boost::optional<ValidationActionEnum> newAction,
                                       boost::optional<Collection::Validator> newValidator,
                                       boost::optional<bool> newPrepareConstraintValidationLevel) {
    if (newPrepareConstraintValidationLevel.has_value() && *newPrepareConstraintValidationLevel &&
        opts.validationLevel == ValidationLevelEnum::constraint) {
        LOGV2_WARNING(12371301,
                      "prepareConstraintValidationLevel is being ignored because the collection is "
                      "already at 'constraint' validationLevel");
    }

    auto validationAction = validationActionOrCurrent(opts, newAction);
    auto validationLevel = validationLevelOrCurrent(opts, newLevel);
    if (opts.encryptedFieldConfig) {
        if (!validationLevelIsMandatory(validationLevel)) {
            return Status(
                ErrorCodes::BadValue,
                "Validation levels other than 'strict' or 'constraint' are not allowed on "
                "encrypted collections");
        }
        if (validationAction == ValidationActionEnum::warn ||
            validationAction == ValidationActionEnum::errorAndLog) {
            return Status(
                ErrorCodes::BadValue,
                "Validation action of 'warn' and 'errorAndLog' are not allowed on encrypted "
                "collections");
        }
    }
    if (opts.uuid && opts.prepareConstraintValidationLevel && newValidator) {
        return Status(
            ErrorCodes::BadValue,
            "Validator cannot be changed while prepareConstraintValidationLevel is set. "
            "To make validator changes, first run: db.runCommand({collMod: \"<collection>\""
            ", prepareConstraintValidationLevel: false})");
    }

    // The prepare phase is only meaningful when the collection already rejects non-conforming
    // documents, i.e. validationAction is not 'warn'. With 'warn' the collection keeps accepting
    // non-conforming documents while preparing, defeating the purpose of the flag.
    if (newPrepareConstraintValidationLevel.value_or(false) &&
        validationAction == ValidationActionEnum::warn) {
        return Status(
            ErrorCodes::BadValue,
            "Cannot set prepareConstraintValidationLevel to true when Validation action is 'warn'");
    }

    // Restrictions when validationLevel is (or becomes) 'constraint':
    // - validationAction must not be 'warn', either before or after the change; 'warn' admits
    //   non-conforming documents that the upgrade to 'constraint' would leave in place.
    // - When starting from constraint, the validator cannot be changed.
    // Upgrade to 'constraint' preconditions (existing collections only):
    // - validationLevel must be 'strict'
    // - the validator cannot be changed in the upgrading collMod
    // - prepareConstraintValidationLevel must already be set
    if (validationLevel == ValidationLevelEnum::constraint) {
        if (validationAction == ValidationActionEnum::warn ||
            opts.validationAction == ValidationActionEnum::warn) {
            return Status(ErrorCodes::BadValue,
                          "Validation action of 'warn' is not allowed when Validation level is "
                          "'constraint'. Change validationAction to 'error' before upgrading");
        }

        if (opts.uuid) {  // existing collection
            if (opts.validationLevel == ValidationLevelEnum::constraint && newValidator) {
                return Status(ErrorCodes::BadValue,
                              "Validator cannot be changed when Validation level is 'constraint'");
            }
            if (newLevel == ValidationLevelEnum::constraint &&
                opts.validationLevel != ValidationLevelEnum::constraint) {
                // Only enforce upgrade preconditions when actually changing the level. When the
                // collection is already at constraint the command is a no-op; validator changes
                // in that case are still blocked by the check above.
                if (!opts.validationLevel || opts.validationLevel != ValidationLevelEnum::strict) {
                    return Status(
                        ErrorCodes::BadValue,
                        "validationLevel can only be changed to 'constraint' from 'strict'");
                }
                if (newValidator) {
                    return Status(ErrorCodes::BadValue,
                                  "Cannot change the validator in the same collMod that upgrades "
                                  "validationLevel to 'constraint'");
                }
                if (!opts.prepareConstraintValidationLevel) {
                    return Status(
                        ErrorCodes::BadValue,
                        "Cannot upgrade validationLevel to 'constraint' without first setting "
                        "prepareConstraintValidationLevel to true");
                }
            }
        }
    }
    return Status::OK();
}

Status checkConstraintUpgradePreconditions(const CollectionOptions& opts,
                                           boost::optional<ValidationActionEnum> newAction,
                                           bool hasNewValidator) {
    // Only the presence of a new validator matters to the checks, not its contents.
    boost::optional<Collection::Validator> newValidator;
    if (hasNewValidator) {
        newValidator.emplace();
    }
    return checkValidationOptionsCanBeUsed(opts,
                                           ValidationLevelEnum::constraint,
                                           newAction,
                                           std::move(newValidator),
                                           /*newPrepareConstraintValidationLevel=*/boost::none);
}

std::pair<bool, MatchExpressionParser::AllowedFeatureSet> mustReparseValidator(
    boost::optional<ValidationLevelEnum> newLevel,
    boost::optional<ValidationActionEnum> newAction) {

    auto allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;

    if (!newAction && !newLevel) {
        return {false, allowedFeatures};
    }
    if (newLevel == ValidationLevelEnum::moderate || newAction == ValidationActionEnum::warn ||
        newAction == ValidationActionEnum::errorAndLog) {
        allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;
    }
    return {true, allowedFeatures};
}

}  // namespace mongo

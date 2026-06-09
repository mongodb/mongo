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

    // Upgrade to 'constraint' restrictions:
    // - validationAction must not be 'warn'
    // - validationLevel must be 'strict'
    // - cannot change validator in the upgrading collmod
    // Restrictions when starting from constraint:
    // - validator cannot be changed
    // - validationAction cannot be set to 'warn'
    if (validationLevel == ValidationLevelEnum::constraint) {
        if (validationAction == ValidationActionEnum::warn) {
            return Status(
                ErrorCodes::BadValue,
                "Validation action of 'warn' is not allowed when Validation level is 'constraint'");
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

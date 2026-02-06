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

namespace mongo {

Status checkValidationOptionsCanBeUsed(const CollectionOptions& opts,
                                       boost::optional<ValidationLevelEnum> newLevel,
                                       boost::optional<ValidationActionEnum> newAction,
                                       boost::optional<Collection::Validator> newValidator) {

    auto validationAction = validationActionOrCurrent(opts, newAction);
    auto validationLevel = validationLevelOrCurrent(opts, newLevel);
    if (opts.encryptedFieldConfig) {
        if (!validationLevelIsMandatory(validationLevel)) {
            return Status(ErrorCodes::BadValue,
                          "Validation levels other than 'strict' or 'validated' are not allowed on "
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
    if (validationLevel == ValidationLevelEnum::validated) {
        if (validationAction == ValidationActionEnum::warn) {
            return Status(
                ErrorCodes::BadValue,
                "Validation action of 'warn' is not allowed when Validation level is 'validated'");
        }
        if (opts.uuid) {  // existing collection
            if (opts.validationLevel != ValidationLevelEnum::validated) {
                return Status(
                    ErrorCodes::BadValue,
                    "Validation level 'validated' can not be set on existing collections.");
            }
            if (newValidator) {
                return Status(ErrorCodes::BadValue,
                              "Validator can not be changed when Validation level is 'validated'");
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

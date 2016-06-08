/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_factory_icu.h"

#include <unicode/coll.h>
#include <unicode/errorcode.h>
#include <unicode/ucol.h>
#include <unicode/uvernum.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/query/collation/collator_interface_icu.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

const char kFallbackLocaleName[] = "root";

// Helper methods for converting between ICU attributes and types used by CollationSpec.

UColAttributeValue boolToAttribute(bool value) {
    if (value) {
        return UCOL_ON;
    }
    return UCOL_OFF;
}

bool attributeToBool(UColAttributeValue attribute) {
    switch (attribute) {
        case UCOL_ON:
            return true;
        case UCOL_OFF:
            return false;
        default:
            MONGO_UNREACHABLE;
    }
}

UColAttributeValue getCaseFirstAttribute(CollationSpec::CaseFirstType caseFirst) {
    switch (caseFirst) {
        case CollationSpec::CaseFirstType::kUpper:
            return UCOL_UPPER_FIRST;
        case CollationSpec::CaseFirstType::kLower:
            return UCOL_LOWER_FIRST;
        case CollationSpec::CaseFirstType::kOff:
            return UCOL_OFF;
    }

    MONGO_UNREACHABLE;
}

CollationSpec::CaseFirstType getCaseFirstFromAttribute(UColAttributeValue caseFirstAttribute) {
    switch (caseFirstAttribute) {
        case UCOL_UPPER_FIRST:
            return CollationSpec::CaseFirstType::kUpper;
        case UCOL_LOWER_FIRST:
            return CollationSpec::CaseFirstType::kLower;
        case UCOL_OFF:
            return CollationSpec::CaseFirstType::kOff;
        default:
            MONGO_UNREACHABLE;
    }
}

UColAttributeValue getStrengthAttribute(CollationSpec::StrengthType strength) {
    switch (strength) {
        case CollationSpec::StrengthType::kPrimary:
            return UCOL_PRIMARY;
        case CollationSpec::StrengthType::kSecondary:
            return UCOL_SECONDARY;
        case CollationSpec::StrengthType::kTertiary:
            return UCOL_TERTIARY;
        case CollationSpec::StrengthType::kQuaternary:
            return UCOL_QUATERNARY;
        case CollationSpec::StrengthType::kIdentical:
            return UCOL_IDENTICAL;
    }

    MONGO_UNREACHABLE;
}

CollationSpec::StrengthType getStrengthFromAttribute(UColAttributeValue strengthAttribute) {
    switch (strengthAttribute) {
        case UCOL_PRIMARY:
            return CollationSpec::StrengthType::kPrimary;
        case UCOL_SECONDARY:
            return CollationSpec::StrengthType::kSecondary;
        case UCOL_TERTIARY:
            return CollationSpec::StrengthType::kTertiary;
        case UCOL_QUATERNARY:
            return CollationSpec::StrengthType::kQuaternary;
        case UCOL_IDENTICAL:
            return CollationSpec::StrengthType::kIdentical;
        default:
            MONGO_UNREACHABLE;
    }
}

UColAttributeValue getAlternateAttribute(CollationSpec::AlternateType alternate) {
    switch (alternate) {
        case CollationSpec::AlternateType::kNonIgnorable:
            return UCOL_NON_IGNORABLE;
        case CollationSpec::AlternateType::kShifted:
            return UCOL_SHIFTED;
    }

    MONGO_UNREACHABLE;
}

CollationSpec::AlternateType getAlternateFromAttribute(UColAttributeValue alternateAttribute) {
    switch (alternateAttribute) {
        case UCOL_NON_IGNORABLE:
            return CollationSpec::AlternateType::kNonIgnorable;
        case UCOL_SHIFTED:
            return CollationSpec::AlternateType::kShifted;
        default:
            MONGO_UNREACHABLE;
    }
}

UColReorderCode getMaxVariableReorderCode(CollationSpec::MaxVariableType maxVariable) {
    switch (maxVariable) {
        case CollationSpec::MaxVariableType::kPunct:
            return UCOL_REORDER_CODE_PUNCTUATION;
        case CollationSpec::MaxVariableType::kSpace:
            return UCOL_REORDER_CODE_SPACE;
    }

    MONGO_UNREACHABLE;
}

CollationSpec::MaxVariableType getMaxVariableFromReorderCode(
    UColReorderCode maxVariableReorderCode) {
    switch (maxVariableReorderCode) {
        case UCOL_REORDER_CODE_PUNCTUATION:
            return CollationSpec::MaxVariableType::kPunct;
        case UCOL_REORDER_CODE_SPACE:
            return CollationSpec::MaxVariableType::kSpace;
        default:
            MONGO_UNREACHABLE;
    }
}

// Helper methods for converting from constants to types used by CollationSpec.

StatusWith<CollationSpec::CaseFirstType> stringToCaseFirstType(const std::string& caseFirst) {
    if (caseFirst == CollationSpec::kCaseFirstUpper) {
        return CollationSpec::CaseFirstType::kUpper;
    } else if (caseFirst == CollationSpec::kCaseFirstLower) {
        return CollationSpec::CaseFirstType::kLower;
    } else if (caseFirst == CollationSpec::kCaseFirstOff) {
        return CollationSpec::CaseFirstType::kOff;
    } else {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Field '" << CollationSpec::kCaseFirstField << "' must be '"
                              << CollationSpec::kCaseFirstUpper
                              << "', '"
                              << CollationSpec::kCaseFirstLower
                              << "', or '"
                              << CollationSpec::kCaseFirstOff
                              << "'. Got: "
                              << caseFirst};
    }
}

StatusWith<CollationSpec::StrengthType> integerToStrengthType(long long strength) {
    switch (strength) {
        case static_cast<int>(CollationSpec::StrengthType::kPrimary):
            return CollationSpec::StrengthType::kPrimary;
        case static_cast<int>(CollationSpec::StrengthType::kSecondary):
            return CollationSpec::StrengthType::kSecondary;
        case static_cast<int>(CollationSpec::StrengthType::kTertiary):
            return CollationSpec::StrengthType::kTertiary;
        case static_cast<int>(CollationSpec::StrengthType::kQuaternary):
            return CollationSpec::StrengthType::kQuaternary;
        case static_cast<int>(CollationSpec::StrengthType::kIdentical):
            return CollationSpec::StrengthType::kIdentical;
    }
    return {ErrorCodes::FailedToParse,
            str::stream() << "Field '" << CollationSpec::kStrengthField
                          << "' must be an integer 1 through 5. Got: "
                          << strength};
}

StatusWith<CollationSpec::AlternateType> stringToAlternateType(const std::string& alternate) {
    if (alternate == CollationSpec::kAlternateNonIgnorable) {
        return CollationSpec::AlternateType::kNonIgnorable;
    } else if (alternate == CollationSpec::kAlternateShifted) {
        return CollationSpec::AlternateType::kShifted;
    } else {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Field '" << CollationSpec::kAlternateField << "' must be '"
                              << CollationSpec::kAlternateNonIgnorable
                              << "' or '"
                              << CollationSpec::kAlternateShifted
                              << "'. Got: "
                              << alternate};
    }
}

StatusWith<CollationSpec::MaxVariableType> stringToMaxVariableType(const std::string& maxVariable) {
    if (maxVariable == CollationSpec::kMaxVariablePunct) {
        return CollationSpec::MaxVariableType::kPunct;
    } else if (maxVariable == CollationSpec::kMaxVariableSpace) {
        return CollationSpec::MaxVariableType::kSpace;
    } else {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Field '" << CollationSpec::kMaxVariableField << "' must be '"
                              << CollationSpec::kMaxVariablePunct
                              << "' or '"
                              << CollationSpec::kMaxVariableSpace
                              << "'. Got: "
                              << maxVariable};
    }
}

// Extracts the collation options from 'spec', performs validation, and sets the options in
// 'icuCollator' and the output CollationSpec.
// Sets the localeID in the CollationSpec to 'localeID'.
StatusWith<CollationSpec> parseToCollationSpec(const BSONObj& spec,
                                               const std::string& localeID,
                                               icu::Collator* icuCollator) {
    CollationSpec parsedSpec;

    // Set the localeID.
    parsedSpec.localeID = localeID;

    // Count the number of fields we have parsed from 'spec'.
    // Begin this at 1 since the locale has already been parsed.
    int parsedFields = 1;

    // Set caseLevel.
    Status parseStatus =
        bsonExtractBooleanField(spec, CollationSpec::kCaseLevelField, &parsedSpec.caseLevel);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue caseLevelAttribute = icuCollator->getAttribute(UCOL_CASE_LEVEL, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kCaseLevelField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.caseLevel = attributeToBool(caseLevelAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(UCOL_CASE_LEVEL, boolToAttribute(parsedSpec.caseLevel), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kCaseLevelField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set caseFirst.
    std::string caseFirst;
    parseStatus = bsonExtractStringField(spec, CollationSpec::kCaseFirstField, &caseFirst);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue caseFirstAttribute = icuCollator->getAttribute(UCOL_CASE_FIRST, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kCaseFirstField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.caseFirst = getCaseFirstFromAttribute(caseFirstAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;

        auto caseFirstStatus = stringToCaseFirstType(caseFirst);
        if (!caseFirstStatus.isOK()) {
            return caseFirstStatus.getStatus();
        }
        parsedSpec.caseFirst = caseFirstStatus.getValue();

        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_CASE_FIRST, getCaseFirstAttribute(parsedSpec.caseFirst), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kCaseFirstField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set strength.
    long long strength;
    parseStatus = bsonExtractIntegerField(spec, CollationSpec::kStrengthField, &strength);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue strengthAttribute = icuCollator->getAttribute(UCOL_STRENGTH, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kStrengthField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.strength = getStrengthFromAttribute(strengthAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;

        auto strengthStatus = integerToStrengthType(strength);
        if (!strengthStatus.isOK()) {
            return strengthStatus.getStatus();
        }
        parsedSpec.strength = strengthStatus.getValue();

        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(UCOL_STRENGTH, getStrengthAttribute(parsedSpec.strength), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kStrengthField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set numericOrdering.
    parseStatus = bsonExtractBooleanField(
        spec, CollationSpec::kNumericOrderingField, &parsedSpec.numericOrdering);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue numericOrderingAttribute =
            icuCollator->getAttribute(UCOL_NUMERIC_COLLATION, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kNumericOrderingField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.numericOrdering = attributeToBool(numericOrderingAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_NUMERIC_COLLATION, boolToAttribute(parsedSpec.numericOrdering), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kNumericOrderingField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set alternate.
    std::string alternate;
    parseStatus = bsonExtractStringField(spec, CollationSpec::kAlternateField, &alternate);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue alternateAttribute =
            icuCollator->getAttribute(UCOL_ALTERNATE_HANDLING, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kAlternateField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.alternate = getAlternateFromAttribute(alternateAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;

        auto alternateStatus = stringToAlternateType(alternate);
        if (!alternateStatus.isOK()) {
            return alternateStatus.getStatus();
        }
        parsedSpec.alternate = alternateStatus.getValue();

        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_ALTERNATE_HANDLING, getAlternateAttribute(parsedSpec.alternate), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kAlternateField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set maxVariable.
    std::string maxVariable;
    parseStatus = bsonExtractStringField(spec, CollationSpec::kMaxVariableField, &maxVariable);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        parsedSpec.maxVariable = getMaxVariableFromReorderCode(icuCollator->getMaxVariable());
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;

        auto maxVariableStatus = stringToMaxVariableType(maxVariable);
        if (!maxVariableStatus.isOK()) {
            return maxVariableStatus.getStatus();
        }
        parsedSpec.maxVariable = maxVariableStatus.getValue();

        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setMaxVariable(getMaxVariableReorderCode(parsedSpec.maxVariable), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kMaxVariableField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set normalization.
    parseStatus = bsonExtractBooleanField(
        spec, CollationSpec::kNormalizationField, &parsedSpec.normalization);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue normalizationAttribute =
            icuCollator->getAttribute(UCOL_NORMALIZATION_MODE, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kNormalizationField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.normalization = attributeToBool(normalizationAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_NORMALIZATION_MODE, boolToAttribute(parsedSpec.normalization), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kNormalizationField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Set backwards.
    parseStatus =
        bsonExtractBooleanField(spec, CollationSpec::kBackwardsField, &parsedSpec.backwards);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue backwardsAttribute =
            icuCollator->getAttribute(UCOL_FRENCH_COLLATION, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << CollationSpec::kBackwardsField
                                  << "' attribute from icu::Collator: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
        parsedSpec.backwards = attributeToBool(backwardsAttribute);
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        ++parsedFields;
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_FRENCH_COLLATION, boolToAttribute(parsedSpec.backwards), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << CollationSpec::kBackwardsField
                                  << "' attribute: "
                                  << icuError.errorName()
                                  << ". Collation spec: "
                                  << spec};
        }
    }

    // Populate the spec with the ICU version information.
    parsedSpec.version = U_ICU_VERSION;

    // Parse the version string, if present in the spec. If the version string does not match the
    // ICU version currently in use we must return an "IncompatibleCollationVersion" error.
    std::string specVersionStr;
    parseStatus = bsonExtractStringField(spec, CollationSpec::kVersionField, &specVersionStr);
    if (parseStatus == ErrorCodes::NoSuchKey) {
        // The BSON spec does not have any particular version. We've already populated it with the
        // ICU version string above.
        invariant(!parsedSpec.version.empty());
    } else if (!parseStatus.isOK()) {
        return parseStatus;
    } else {
        if (specVersionStr != parsedSpec.version) {
            return {ErrorCodes::IncompatibleCollationVersion,
                    str::stream() << "Requested collation version " << specVersionStr
                                  << " but the only available collator version was "
                                  << parsedSpec.version
                                  << ". Requested collation spec: "
                                  << spec};
        }

        ++parsedFields;
    }

    // Check for unknown fields.
    invariant(parsedFields <= spec.nFields());
    if (parsedFields < spec.nFields()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Collation spec contains unknown field. Collation spec: " << spec};
    }

    return parsedSpec;
}

// Extracts the localeID from 'spec', if present.
StatusWith<std::string> parseLocaleID(const BSONObj& spec) {
    std::string localeID;
    Status status = bsonExtractStringField(spec, CollationSpec::kLocaleField, &localeID);
    if (!status.isOK()) {
        return status;
    }
    if (localeID.find('\0') != std::string::npos) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << CollationSpec::kLocaleField
                              << "' cannot contain null byte. Collation spec: "
                              << spec};
    }
    return localeID;
}

// Returns a non-OK status if any part of the locale ID is invalid or not recognized by ICU.
Status validateLocaleID(const BSONObj& spec,
                        const std::string& originalID,
                        const icu::Collator& collator) {
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale collatorLocale = collator.getLocale(ULOC_VALID_LOCALE, status);
    if (U_FAILURE(status)) {
        icu::ErrorCode icuError;
        icuError.set(status);
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to get locale from icu::Collator: " << icuError.errorName()
                              << ". Collation spec: "
                              << spec};
    }

    if (originalID.empty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << CollationSpec::kLocaleField
                              << "' cannot be the empty string in: "
                              << spec};
    }

    // Check that each component of the locale ID is recognized by ICU. If ICU 1) cannot parse the
    // locale id, 2) has to do any sort of locale id canonicalization, 3) does not recognize some
    // component of the locale, or 4) has no data to support some component of the locale, then the
    // resulting icu::Locale name will not match the requested locale. In this case we return an
    // error to the user. In the error message to the user, we report the locale that ICU *would
    // have* used, which the application can supply as an alternative.
    const char* collatorLocaleName = collatorLocale.getName();
    if (StringData(originalID) != StringData(collatorLocaleName)) {
        str::stream ss;
        ss << "Field '" << CollationSpec::kLocaleField << "' is invalid in: " << spec;

        if (!str::equals(kFallbackLocaleName, collatorLocaleName) &&
            !str::equals("", collatorLocaleName)) {
            ss << ". Did you mean '" << collatorLocaleName << "'?";
        }

        return {ErrorCodes::BadValue, ss};
    }

    return Status::OK();
}

}  // namespace

StatusWith<std::unique_ptr<CollatorInterface>> CollatorFactoryICU::makeFromBSON(
    const BSONObj& spec) {
    // Parse the locale ID out of the spec.
    auto parsedLocaleID = parseLocaleID(spec);
    if (!parsedLocaleID.isOK()) {
        return parsedLocaleID.getStatus();
    }

    // If spec = {locale: "simple"}, return a null pointer. A null CollatorInterface indicates
    // simple binary compare.
    if (parsedLocaleID.getValue() == CollationSpec::kSimpleBinaryComparison) {
        if (spec.nFields() > 1) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "If " << CollationSpec::kLocaleField << "="
                                  << CollationSpec::kSimpleBinaryComparison
                                  << ", no other fields should be present in: "
                                  << spec};
        }
        return {nullptr};
    }

    // Construct an icu::Locale.
    auto userLocale = icu::Locale::createFromName(parsedLocaleID.getValue().c_str());
    if (userLocale.isBogus()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << CollationSpec::kLocaleField << "' is not valid in: "
                              << spec};
    }

    // Construct an icu::Collator.
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> icuCollator(icu::Collator::createInstance(userLocale, status));
    if (U_FAILURE(status)) {
        icu::ErrorCode icuError;
        icuError.set(status);
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to create collator: " << icuError.errorName()
                              << ". Collation spec: "
                              << spec};
    }

    Status localeValidationStatus = validateLocaleID(spec, parsedLocaleID.getValue(), *icuCollator);
    if (!localeValidationStatus.isOK()) {
        return localeValidationStatus;
    }

    // Construct a CollationSpec using the options provided in spec or the defaults in icuCollator.
    // Use userLocale.getName() for the localeID, since it is canonicalized and includes options.
    auto parsedSpec = parseToCollationSpec(spec, userLocale.getName(), icuCollator.get());
    if (!parsedSpec.isOK()) {
        return parsedSpec.getStatus();
    }

    auto mongoCollator = stdx::make_unique<CollatorInterfaceICU>(std::move(parsedSpec.getValue()),
                                                                 std::move(icuCollator));
    return {std::move(mongoCollator)};
}

}  // namespace mongo

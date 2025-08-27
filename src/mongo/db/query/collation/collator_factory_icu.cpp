/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/collation/collator_factory_icu.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface_icu.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <unicode/coll.h>
#include <unicode/errorcode.h>
#include <unicode/locid.h>
#include <unicode/ucol.h>
#include <unicode/uloc.h>
#include <unicode/utypes.h>
#include <unicode/uvernum.h>

namespace mongo {

namespace {

constexpr StringData kFallbackLocaleName = "root"_sd;

// Helper methods for converting between ICU attributes and types used by Collation.

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

UColAttributeValue getCaseFirstAttribute(CollationCaseFirstEnum caseFirst) {
    switch (caseFirst) {
        case CollationCaseFirstEnum::kUpper:
            return UCOL_UPPER_FIRST;
        case CollationCaseFirstEnum::kLower:
            return UCOL_LOWER_FIRST;
        case CollationCaseFirstEnum::kOff:
            return UCOL_OFF;
        default:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

CollationCaseFirstEnum getCaseFirstFromAttribute(UColAttributeValue caseFirstAttribute) {
    switch (caseFirstAttribute) {
        case UCOL_UPPER_FIRST:
            return CollationCaseFirstEnum::kUpper;
        case UCOL_LOWER_FIRST:
            return CollationCaseFirstEnum::kLower;
        case UCOL_OFF:
            return CollationCaseFirstEnum::kOff;
        default:
            MONGO_UNREACHABLE;
    }
}

UColAttributeValue getStrengthAttribute(int strength) {
    switch (static_cast<CollationStrength>(strength)) {
        case CollationStrength::kPrimary:
            return UCOL_PRIMARY;
        case CollationStrength::kSecondary:
            return UCOL_SECONDARY;
        case CollationStrength::kTertiary:
            return UCOL_TERTIARY;
        case CollationStrength::kQuaternary:
            return UCOL_QUATERNARY;
        case CollationStrength::kIdentical:
            return UCOL_IDENTICAL;
        default:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

int getStrengthFromAttribute(UColAttributeValue strengthAttribute) {
    switch (strengthAttribute) {
        case UCOL_PRIMARY:
            return static_cast<int>(CollationStrength::kPrimary);
        case UCOL_SECONDARY:
            return static_cast<int>(CollationStrength::kSecondary);
        case UCOL_TERTIARY:
            return static_cast<int>(CollationStrength::kTertiary);
        case UCOL_QUATERNARY:
            return static_cast<int>(CollationStrength::kQuaternary);
        case UCOL_IDENTICAL:
            return static_cast<int>(CollationStrength::kIdentical);
        default:
            MONGO_UNREACHABLE;
    }
}

UColAttributeValue getAlternateAttribute(CollationAlternateEnum alternate) {
    switch (alternate) {
        case CollationAlternateEnum::kNonIgnorable:
            return UCOL_NON_IGNORABLE;
        case CollationAlternateEnum::kShifted:
            return UCOL_SHIFTED;
        default:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

CollationAlternateEnum getAlternateFromAttribute(UColAttributeValue alternateAttribute) {
    switch (alternateAttribute) {
        case UCOL_NON_IGNORABLE:
            return CollationAlternateEnum::kNonIgnorable;
        case UCOL_SHIFTED:
            return CollationAlternateEnum::kShifted;
        default:
            MONGO_UNREACHABLE;
    }
}

UColReorderCode getMaxVariableReorderCode(CollationMaxVariableEnum maxVariable) {
    switch (maxVariable) {
        case CollationMaxVariableEnum::kPunct:
            return UCOL_REORDER_CODE_PUNCTUATION;
        case CollationMaxVariableEnum::kSpace:
            return UCOL_REORDER_CODE_SPACE;
        default:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

CollationMaxVariableEnum getMaxVariableFromReorderCode(UColReorderCode maxVariableReorderCode) {
    switch (maxVariableReorderCode) {
        case UCOL_REORDER_CODE_PUNCTUATION:
            return CollationMaxVariableEnum::kPunct;
        case UCOL_REORDER_CODE_SPACE:
            return CollationMaxVariableEnum::kSpace;
        default:
            MONGO_UNREACHABLE;
    }
}

// Sets the Collation's localeID to 'localeID'. For each collation option, if the user specified the
// option then set it on icuCollation, otherwise copy icuCollation's default to the Collation.
Status updateCollationSpecFromICUCollator(const BSONObj& spec,
                                          const std::string& localeID,
                                          icu::Collator* icuCollator,
                                          Collation* collation) {
    // Set the localeID.
    collation->setLocale(localeID);

    // Set caseLevel.
    if (!spec.hasField(Collation::kCaseLevelFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue caseLevelAttribute = icuCollator->getAttribute(UCOL_CASE_LEVEL, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kCaseLevelFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setCaseLevel(attributeToBool(caseLevelAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_CASE_LEVEL, boolToAttribute(collation->getCaseLevel()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kCaseLevelFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set caseFirst.
    if (!spec.hasField(Collation::kCaseFirstFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue caseFirstAttribute = icuCollator->getAttribute(UCOL_CASE_FIRST, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kCaseFirstFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setCaseFirst(getCaseFirstFromAttribute(caseFirstAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_CASE_FIRST, getCaseFirstAttribute(collation->getCaseFirst()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kCaseFirstFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set strength.
    if (!spec.hasField(Collation::kStrengthFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue strengthAttribute = icuCollator->getAttribute(UCOL_STRENGTH, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kStrengthFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setStrength(getStrengthFromAttribute(strengthAttribute));
    } else {
        try {
            // For backwards compatibility, "strength" is parsed from any int, long, or double.
            // Check it matches an enum value.
            CollationStrength_parse(collation->getStrength(),
                                    IDLParserContext{"collation.strength"});
        } catch (const DBException& exc) {
            return exc.toStatus();
        }

        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_STRENGTH, getStrengthAttribute(collation->getStrength()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kStrengthFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set numericOrdering.
    if (!spec.hasField(Collation::kNumericOrderingFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue numericOrderingAttribute =
            icuCollator->getAttribute(UCOL_NUMERIC_COLLATION, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kNumericOrderingFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setNumericOrdering(attributeToBool(numericOrderingAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_NUMERIC_COLLATION, boolToAttribute(collation->getNumericOrdering()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kNumericOrderingFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set alternate.
    if (!spec.hasField(Collation::kAlternateFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue alternateAttribute =
            icuCollator->getAttribute(UCOL_ALTERNATE_HANDLING, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kAlternateFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setAlternate(getAlternateFromAttribute(alternateAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_ALTERNATE_HANDLING, getAlternateAttribute(collation->getAlternate()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kAlternateFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set maxVariable.
    if (!spec.hasField(Collation::kMaxVariableFieldName)) {
        collation->setMaxVariable(getMaxVariableFromReorderCode(icuCollator->getMaxVariable()));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setMaxVariable(getMaxVariableReorderCode(collation->getMaxVariable()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kMaxVariableFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set normalization.
    if (!spec.hasField(Collation::kNormalizationFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue normalizationAttribute =
            icuCollator->getAttribute(UCOL_NORMALIZATION_MODE, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kNormalizationFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setNormalization(attributeToBool(normalizationAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        icuCollator->setAttribute(
            UCOL_NORMALIZATION_MODE, boolToAttribute(collation->getNormalization()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kNormalizationFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    // Set backwards.
    if (!spec.hasField(Collation::kBackwardsFieldName)) {
        UErrorCode status = U_ZERO_ERROR;
        UColAttributeValue backwardsAttribute =
            icuCollator->getAttribute(UCOL_FRENCH_COLLATION, status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to get '" << Collation::kBackwardsFieldName
                                  << "' attribute from icu::Collator: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
        collation->setBackwards(attributeToBool(backwardsAttribute));
    } else {
        UErrorCode status = U_ZERO_ERROR;
        // collation->getBackwards should be engaged if spec has a "backwards" field.
        invariant(collation->getBackwards().has_value());
        icuCollator->setAttribute(
            UCOL_FRENCH_COLLATION, boolToAttribute(collation->getBackwards()), status);
        if (U_FAILURE(status)) {
            icu::ErrorCode icuError;
            icuError.set(status);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to set '" << Collation::kBackwardsFieldName
                                  << "' attribute: " << icuError.errorName()
                                  << ". Collation spec: " << spec};
        }
    }

    if (!collation->getVersion()) {
        collation->setVersion(StringData(U_ICU_VERSION));
    } else {
        if (U_ICU_VERSION != *collation->getVersion()) {
            return {ErrorCodes::IncompatibleCollationVersion,
                    str::stream() << "Requested collation version " << collation->getVersion()
                                  << " but the only available collator version was "
                                  << U_ICU_VERSION << ". Requested collation spec: " << spec};
        }
    }

    return Status::OK();
}

// Returns a non-OK status if any part of the locale ID is invalid or not recognized by ICU.
Status validateLocaleID(const BSONObj& spec, StringData originalID, const icu::Collator& collator) {
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale collatorLocale = collator.getLocale(ULOC_VALID_LOCALE, status);
    if (U_FAILURE(status)) {
        icu::ErrorCode icuError;
        icuError.set(status);
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to get locale from icu::Collator: " << icuError.errorName()
                              << ". Collation spec: " << spec};
    }

    if (originalID.empty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << Collation::kLocaleFieldName
                              << "' cannot be the empty string in: " << spec};
    }

    // Check that each component of the locale ID is recognized by ICU. If ICU 1) cannot parse the
    // locale id, 2) has to do any sort of locale id canonicalization, 3) does not recognize some
    // component of the locale, or 4) has no data to support some component of the locale, then the
    // resulting icu::Locale name will not match the requested locale. In this case we return an
    // error to the user. In the error message to the user, we report the locale that ICU *would
    // have* used, which the application can supply as an alternative.
    auto collatorLocaleName = StringData(collatorLocale.getName());
    if (originalID != collatorLocaleName) {
        str::stream ss;
        ss << "Field '" << Collation::kLocaleFieldName << "' is invalid in: " << spec;

        if ((collatorLocaleName != kFallbackLocaleName) && !collatorLocaleName.empty()) {
            ss << ". Did you mean '" << collatorLocaleName << "'?";
        }

        return {ErrorCodes::BadValue, ss};
    }

    return Status::OK();
}

// Returns a non-OK status if 'spec' contains any invalid combinations of options.
Status validateCollationSpec(const Collation& collation, const BSONObj& spec) {
    // The backwards option specifically means backwards secondary weighting, and therefore only
    // affects the secondary comparison level. It has no effect at strength 1.
    if (collation.getBackwards() &&
        static_cast<CollationStrength>(collation.getStrength()) == CollationStrength::kPrimary) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << Collation::kBackwardsFieldName << "' is invalid with '"
                              << Collation::kStrengthFieldName << "' of "
                              << static_cast<int>(CollationStrength::kPrimary) << " in: " << spec};
    }

    // The caseFirst option only affects tertiary level or caseLevel comparisons. It will have no
    // affect if caseLevel is off and strength is 1 or 2.
    if (collation.getCaseFirst() != CollationCaseFirstEnum::kOff && !collation.getCaseLevel() &&
        (static_cast<CollationStrength>(collation.getStrength()) == CollationStrength::kPrimary ||
         static_cast<CollationStrength>(collation.getStrength()) ==
             CollationStrength::kSecondary)) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << Collation::kCaseFirstFieldName << "' is invalid unless '"
                              << Collation::kCaseLevelFieldName << "' is on or '"
                              << Collation::kStrengthFieldName << "' is greater than "
                              << static_cast<int>(CollationStrength::kSecondary)
                              << " in: " << spec};
    }

    return Status::OK();
}

}  // namespace

StatusWith<std::unique_ptr<CollatorInterface>> CollatorFactoryICU::makeFromBSON(
    const BSONObj& spec) {

    Collation collation;
    try {
        collation = Collation::parse(spec, IDLParserContext{"collation"});
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (collation.getLocale().find('\0') != std::string::npos) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << Collation::kLocaleFieldName
                              << "' cannot contain null byte. Collation spec: " << spec};
    }

    // If spec = {locale: "simple"}, return a null pointer. A null CollatorInterface indicates
    // simple binary compare.
    if (collation.getLocale() == CollationSpec::kSimpleBinaryComparison) {
        return {nullptr};
    }

    // Construct an icu::Locale.
    auto userLocale = icu::Locale::createFromName(std::string{collation.getLocale()}.c_str());
    if (userLocale.isBogus()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << Collation::kLocaleFieldName
                              << "' is not valid in: " << spec};
    }

    // Construct an icu::Collator.
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> icuCollator(icu::Collator::createInstance(userLocale, status));
    if (U_FAILURE(status)) {
        icu::ErrorCode icuError;
        icuError.set(status);
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to create collator: " << icuError.errorName()
                              << ". Collation spec: " << spec};
    }

    Status localeValidationStatus = validateLocaleID(spec, collation.getLocale(), *icuCollator);
    if (!localeValidationStatus.isOK()) {
        return localeValidationStatus;
    }

    // Update the Collation's options with the defaults in icuCollator.
    // Use userLocale.getName() for the localeID, since it is canonicalized and includes options.
    auto updateCollationSpecStatus = updateCollationSpecFromICUCollator(
        spec, userLocale.getName(), icuCollator.get(), &collation);
    if (!updateCollationSpecStatus.isOK()) {
        return updateCollationSpecStatus;
    }

    auto validateSpecStatus = validateCollationSpec(collation, spec);
    if (!validateSpecStatus.isOK()) {
        return validateSpecStatus;
    }

    auto mongoCollator =
        std::make_unique<CollatorInterfaceICU>(std::move(collation), std::move(icuCollator));
    return {std::move(mongoCollator)};
}

}  // namespace mongo

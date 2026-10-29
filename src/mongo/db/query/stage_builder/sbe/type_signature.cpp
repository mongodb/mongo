// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/type_signature.h"

namespace mongo::stage_builder {

TypeSignature getTypeSignature(sbe::value::TypeTags type) {
    uint8_t tagIndex = static_cast<uint8_t>(type);
    return TypeSignature{1ull << tagIndex};
}

TypeSignature TypeSignature::kAnyType = TypeSignature{TypeSignature::AllTypesTag{}};

TypeSignature TypeSignature::kBlockType = getTypeSignature(sbe::value::TypeTags::valueBlock);

TypeSignature TypeSignature::kCellType = getTypeSignature(sbe::value::TypeTags::cellBlock);

TypeSignature TypeSignature::kAnyScalarType = TypeSignature::kAnyType.exclude(
    getTypeSignature(sbe::value::TypeTags::cellBlock, sbe::value::TypeTags::valueBlock));

// This constant signature holds all the types that have a BSON counterpart and can
// represent a value stored in the database, excluding all the TypeTags that describe
// internal types like SortSpec, TimeZoneDB, etc...
TypeSignature TypeSignature::kAnyBSONType = getTypeSignature(sbe::value::TypeTags::Nothing,
                                                             sbe::value::TypeTags::NumberInt32,
                                                             sbe::value::TypeTags::NumberInt64,
                                                             sbe::value::TypeTags::NumberDouble,
                                                             sbe::value::TypeTags::NumberDecimal,
                                                             sbe::value::TypeTags::Date,
                                                             sbe::value::TypeTags::Timestamp,
                                                             sbe::value::TypeTags::Boolean,
                                                             sbe::value::TypeTags::Null,
                                                             sbe::value::TypeTags::StringSmall,
                                                             sbe::value::TypeTags::StringBig,
                                                             sbe::value::TypeTags::Array,
                                                             sbe::value::TypeTags::ArraySet,
                                                             sbe::value::TypeTags::ArrayMultiSet,
                                                             sbe::value::TypeTags::Object,
                                                             sbe::value::TypeTags::ObjectId,
                                                             sbe::value::TypeTags::MinKey,
                                                             sbe::value::TypeTags::MaxKey,
                                                             sbe::value::TypeTags::bsonObject,
                                                             sbe::value::TypeTags::bsonArray,
                                                             sbe::value::TypeTags::bsonString,
                                                             sbe::value::TypeTags::bsonSymbol,
                                                             sbe::value::TypeTags::bsonObjectId,
                                                             sbe::value::TypeTags::bsonBinData,
                                                             sbe::value::TypeTags::bsonUndefined,
                                                             sbe::value::TypeTags::bsonRegex,
                                                             sbe::value::TypeTags::bsonJavascript,
                                                             sbe::value::TypeTags::bsonDBPointer,
                                                             sbe::value::TypeTags::bsonCodeWScope);

TypeSignature TypeSignature::kArrayType = getTypeSignature(sbe::value::TypeTags::Array,
                                                           sbe::value::TypeTags::ArraySet,
                                                           sbe::value::TypeTags::ArrayMultiSet,
                                                           sbe::value::TypeTags::bsonArray);
TypeSignature TypeSignature::kBooleanType = getTypeSignature(sbe::value::TypeTags::Boolean);
TypeSignature TypeSignature::kDateTimeType =
    getTypeSignature(sbe::value::TypeTags::Date, sbe::value::TypeTags::Timestamp);
TypeSignature TypeSignature::kNothingType = getTypeSignature(sbe::value::TypeTags::Nothing);
TypeSignature TypeSignature::kNullishType = getTypeSignature(
    sbe::value::TypeTags::Nothing, sbe::value::TypeTags::Null, sbe::value::TypeTags::bsonUndefined);
TypeSignature TypeSignature::kNumericType = getTypeSignature(sbe::value::TypeTags::NumberInt32,
                                                             sbe::value::TypeTags::NumberInt64,
                                                             sbe::value::TypeTags::NumberDecimal,
                                                             sbe::value::TypeTags::NumberDouble);
TypeSignature TypeSignature::kObjectType =
    getTypeSignature(sbe::value::TypeTags::Object, sbe::value::TypeTags::bsonObject);
TypeSignature TypeSignature::kStringType = getTypeSignature(sbe::value::TypeTags::StringSmall,
                                                            sbe::value::TypeTags::StringBig,
                                                            sbe::value::TypeTags::bsonString);

TypeSignature TypeSignature::kCollatableType =
    TypeSignature::kStringType.include(getTypeSignature(sbe::value::TypeTags::bsonSymbol))
        .include(TypeSignature::kArrayType)
        .include(TypeSignature::kObjectType);

std::string TypeSignature::debugString() const {
    if (*this == kAnyType) {
        return "kAnyType";
    }
    if (*this == kAnyScalarType) {
        return "kAnyScalarType";
    }

    return std::bitset<sizeof(MaskType) * 8>(typesMask).to_string();
}

bool TypeSignature::canCompareWith(TypeSignature other) const {
    auto lhsTypes = getBSONTypesFromSignature(*this);
    auto rhsTypes = getBSONTypesFromSignature(other);
    for (auto& lhsTag : lhsTypes) {
        if (lhsTag == sbe::value::TypeTags::Nothing) {
            return false;
        }
        for (auto& rhsTag : rhsTypes) {
            if (rhsTag == sbe::value::TypeTags::Nothing ||
                (lhsTag != rhsTag &&
                 !(sbe::value::isNumber(lhsTag) && sbe::value::isNumber(rhsTag)) &&
                 !(sbe::value::isStringOrSymbol(lhsTag) && sbe::value::isStringOrSymbol(rhsTag)))) {
                return false;
            }
        }
    }
    return true;
}

// Return the set of SBE types encoded in the provided signature.
std::vector<sbe::value::TypeTags> getBSONTypesFromSignature(TypeSignature signature) {
    signature = signature.intersect(TypeSignature::kAnyBSONType);
    std::vector<sbe::value::TypeTags> tags;
    for (size_t i = 0; i < sizeof(TypeSignature::typesMask) * 8; i++) {
        auto tag = static_cast<sbe::value::TypeTags>(i);
        if (getTypeSignature(tag).isSubset(signature)) {
            tags.push_back(tag);
        }
    }
    return tags;
}

}  // namespace mongo::stage_builder

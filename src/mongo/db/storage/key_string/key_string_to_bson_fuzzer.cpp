// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_validate.h"
#include "mongo/db/storage/key_string/key_string.h"

const mongo::Ordering kAllAscending = mongo::Ordering::make(mongo::BSONObj());
const mongo::Ordering kOneDescending = mongo::Ordering::make(BSON("a" << -1));
const auto kV1 = mongo::key_string::Version::V1;
const auto kV0 = mongo::key_string::Version::V0;

uint8_t getZeroType(char val) {
    switch (val % 10) {
        case 0:
            return mongo::key_string::TypeBits::kInt;
        case 1:
            return mongo::key_string::TypeBits::kDouble;
        case 2:
            return mongo::key_string::TypeBits::kLong;
        case 3:
            return mongo::key_string::TypeBits::kNegativeDoubleZero;
        case 4:
            return mongo::key_string::TypeBits::kDecimalZero0xxx;
        case 5:
            return mongo::key_string::TypeBits::kDecimalZero1xxx;
        case 6:
            return mongo::key_string::TypeBits::kDecimalZero2xxx;
        case 7:
            return mongo::key_string::TypeBits::kDecimalZero3xxx;
        case 8:
            return mongo::key_string::TypeBits::kDecimalZero4xxx;
        case 9:
            return mongo::key_string::TypeBits::kDecimalZero5xxx;
        default:
            return 0x00;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    if (Size < 4)
        return 0;
    std::span data(Data, Size);

    const auto version = data[0] % 2 == 0 ? kV0 : kV1;
    const auto ord = data[1] % 2 == 0 ? kAllAscending : kOneDescending;

    mongo::key_string::TypeBits tb(version);

    const size_t len = data[2];
    if (len > data.size() - 3)
        return 0;
    // Data[2] defines the number of types to append to the TypeBits
    // Data[3 + i] defines which types have to be added
    for (size_t i = 0; i < len; i++) {
        char randomType = data[3 + i] & 0xf;
        char randomZeroType = (data[3 + i] & 0xf0) >> 4;
        switch (randomType % 9) {
            case 0:
                tb.appendString();
                break;
            case 1:
                tb.appendSymbol();
                break;
            case 2:
                tb.appendNumberInt();
                break;
            case 3:
                tb.appendNumberLong();
                break;
            case 4:
                tb.appendNumberDouble();
                break;
            case 5:
                tb.appendNumberDecimal();
                break;
            case 6:
                tb.appendZero(getZeroType(randomZeroType));
                break;
            case 7:
                tb.appendDecimalZero(getZeroType(randomZeroType));
                break;
            case 8:
                tb.appendDecimalExponent(getZeroType(randomZeroType));
                break;
            default:
                break;
        }
    }

    auto keyString = data.subspan(len + 2);
    try {
        mongo::BSONObj obj = mongo::key_string::toBsonSafe(keyString, ord, tb);
        // We want to make sure the generated BSON is valid
        auto validationResult = mongo::validateBSON(obj.objdata(), obj.objsize());
        invariant(validationResult.isOK() ||
                  validationResult.code() == mongo::ErrorCodes::NonConformantBSON);
    } catch (const mongo::AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    try {
        mongo::key_string::decodeRecordIdLongAtEnd(keyString);
    } catch (const mongo::AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    try {
        mongo::key_string::decodeRecordIdStrAtEnd(keyString);
    } catch (const mongo::AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    return 0;
}

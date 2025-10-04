/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

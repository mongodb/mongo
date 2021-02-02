/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::value::writeValueToStream.
 */

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {
TEST(ValueSerializeForSorter, Serialize) {
    value::MaterializedRow originalRow(21);

    originalRow.reset(0, true, value::TypeTags::Nothing, 0);
    originalRow.reset(1, true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(33550336));
    originalRow.reset(2, true, value::TypeTags::RecordId, value::bitcastFrom<int64_t>(8589869056));
    originalRow.reset(
        3, true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(137438691328));
    originalRow.reset(4, true, value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.305e18));

    auto [decimalTag, decimalVal] =
        value::makeCopyDecimal(Decimal128("2658455991569831744654692615953842176"));
    originalRow.reset(5, true, decimalTag, decimalVal);

    originalRow.reset(6, true, value::TypeTags::Date, value::bitcastFrom<int64_t>(1234));
    originalRow.reset(7, true, value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(5678));
    originalRow.reset(8, true, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    originalRow.reset(9, true, value::TypeTags::Null, 0);
    originalRow.reset(10, true, value::TypeTags::MinKey, 0);
    originalRow.reset(11, true, value::TypeTags::MaxKey, 0);
    originalRow.reset(12, true, value::TypeTags::bsonUndefined, 0);

    auto [stringTag, stringVal] = value::makeNewString("perfect");
    originalRow.reset(13, true, stringTag, stringVal);

    auto [objectTag, objectVal] = value::makeNewObject();
    originalRow.reset(14, true, objectTag, objectVal);

    auto object = value::getObjectView(objectVal);
    object->push_back("num", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));

    auto [arrayTag, arrayVal] = value::makeNewArray();
    object->push_back("arr", arrayTag, arrayVal);

    auto array = value::getArrayView(arrayVal);
    array->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    array->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));

    auto [arraySetTag, arraySetVal] = value::makeNewArraySet();
    object->push_back("set", arraySetTag, arraySetVal);

    auto arraySet = value::getArraySetView(arraySetVal);
    arraySet->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    arraySet->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5));

    auto [oidTag, oidVal] = value::makeCopyObjectId({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    originalRow.reset(15, true, oidTag, oidVal);

    uint8_t byteArray[] = {8, 7, 6, 5, 4, 3, 2, 1};
    auto bson =
        BSON("obj" << BSON("a" << 1 << "b" << 2) << "arr" << BSON_ARRAY(1 << 2 << 3)  //
                   << "binDataGeneral" << BSONBinData(byteArray, sizeof(byteArray), BinDataGeneral)
                   << "binDataDeprecated"
                   << BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated)
                   << "malformedBinDataDeprecated" << BSONBinData(nullptr, 0, ByteArrayDeprecated));

    auto [bsonObjTag, bsonObjVal] = value::copyValue(
        value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bson["obj"].value()));
    originalRow.reset(16, true, bsonObjTag, bsonObjVal);


    auto [bsonArrayTag, bsonArrayVal] = value::copyValue(
        value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bson["arr"].value()));
    originalRow.reset(17, true, bsonArrayTag, bsonArrayVal);

    auto [bsonBinDataGeneralTag, bsonBinDataGeneralVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataGeneral"].value()));
    originalRow.reset(18, true, bsonBinDataGeneralTag, bsonBinDataGeneralVal);

    auto [bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataDeprecated"].value()));
    originalRow.reset(19, true, bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal);

    KeyString::Builder keyStringBuilder(KeyString::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    auto [keyStringTag, keyStringVal] = value::makeCopyKeyString(keyStringBuilder.getValueCopy());
    originalRow.reset(20, true, keyStringTag, keyStringVal);

    BufBuilder builder;
    originalRow.serializeForSorter(builder);
    auto buffer = builder.release();

    BufReader reader(buffer.get(), buffer.capacity());
    value::MaterializedRow roundTripRow = value::MaterializedRow::deserializeForSorter(reader, {});

    ASSERT(value::MaterializedRowEq()(originalRow, roundTripRow));
}
}  // namespace mongo::sbe

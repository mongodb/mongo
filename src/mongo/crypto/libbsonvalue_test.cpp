/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/crypto/libbsonvalue.h"

#include "mongo/unittest/unittest.h"

#include <bson/bson.h>

namespace mongo {
namespace {

// All of the types that each BSONValue can be
using BSONValue = std::variant<double,
                               std::string,
                               BSONObj,
                               BSONArray,
                               BSONBinData,
                               OID,
                               bool,
                               Date_t,
                               BSONRegEx,
                               BSONCode,
                               int32_t,
                               Timestamp,
                               int64_t,
                               Decimal128>;

struct BSONTypeInfo {
    bson_type_t type;
    // How to append the type to an object builder. Used in objectGenerator().
    std::function<void(BSONObjBuilder&, const std::string&)> appendFunc;
};

// Defines the values of the testing object.
// type name as defined in bsontypes.cpp -> {bson_type_t, appendFunc}
std::map<std::string, BSONTypeInfo> bsonTypeValueMap = {
    {"double",
     {BSON_TYPE_DOUBLE,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, 3.14);
      }}},
    {"string",
     {BSON_TYPE_UTF8,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, "apples");
      }}},
    {"object",
     {BSON_TYPE_DOCUMENT,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, BSON("inner obj" << "banana"));
      }}},
    {"array",
     {BSON_TYPE_ARRAY,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, BSON_ARRAY(0 << 1 << 2));
      }}},
    {"binData",
     {BSON_TYPE_BINARY,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.appendBinData(key, 4, BinDataGeneral, "data");
      }}},
    {"objectId",
     {BSON_TYPE_OID,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, OID("507f1f77bcf86cd799439011"));
      }}},
    {"bool",
     {BSON_TYPE_BOOL,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, true);
      }}},
    {"date",
     {BSON_TYPE_DATE_TIME,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, Date_t::fromMillisSinceEpoch(1730740031460LL));
      }}},
    {"null",
     {BSON_TYPE_NULL,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.appendNull(key);
      }}},
    {"regex",
     {BSON_TYPE_REGEX,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.appendRegex(key, "pattern", "string");
      }}},
    {"javascript",
     {BSON_TYPE_CODE,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.appendCode(key, "code");
      }}},
    {"int",
     {BSON_TYPE_INT32,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, 32);
      }}},
    {"timestamp",
     {BSON_TYPE_TIMESTAMP,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, Timestamp(1730740251, 1));
      }}},
    {"long",
     {BSON_TYPE_INT64,
      [](BSONObjBuilder& builder, const std::string& key) {
          builder.append(key, int64_t(123456789123456789LL));
      }}},
    {"decimal", {BSON_TYPE_DECIMAL128, [](BSONObjBuilder& builder, const std::string& key) {
                     builder.append(key, Decimal128(3.14159));
                 }}}};


// Used by testing function to create an object with all supported types.
BSONObj objectGenerator() {
    BSONObjBuilder builder;

    for (const auto& pair : bsonTypeValueMap) {
        const std::string& fieldName = pair.first;
        const BSONTypeInfo& typeInfo = pair.second;
        typeInfo.appendFunc(builder, fieldName);
    }

    return builder.obj();
}

// Helper function to compare a LibBSONValue to a BSONElement
void assertBSONValue(const LibBSONValue& lbv, bson_type_t expectedType, const BSONElement& elem) {
    ASSERT_EQ(lbv.get()->value_type, expectedType);
    switch (expectedType) {
        case BSON_TYPE_DOUBLE:
            ASSERT_EQ(lbv.get()->value.v_double, elem.Double());
            break;
        case BSON_TYPE_UTF8:
            ASSERT_EQ(lbv.get()->value.v_utf8.len, elem.valueStringData().size());
            ASSERT_EQ(std::string(lbv.get()->value.v_utf8.str), elem.String());
            break;
        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY:
            ASSERT_EQ(lbv.get()->value.v_doc.data_len, elem.Obj().objsize());
            ASSERT_EQ(std::memcmp(
                          lbv.get()->value.v_doc.data, elem.Obj().objdata(), elem.Obj().objsize()),
                      0);
            break;
        case BSON_TYPE_BINARY: {
            int len = 0;
            // Populate len
            elem.binData(len);
            ASSERT_EQ(lbv.get()->value.v_binary.data_len, len);
            ASSERT_EQ(lbv.get()->value.v_binary.subtype, elem.binDataType());
            ASSERT_EQ(std::memcmp(lbv.get()->value.v_binary.data, elem.binData(len), len), 0);
            break;
        }
        case BSON_TYPE_OID: {
            // bson_oid_to_string requires str to be 25 bytes
            char oidStr[OID::kOIDSize * 2 + 1];
            bson_oid_to_string(&lbv.get()->value.v_oid, oidStr);
            ASSERT_EQ(std::string(oidStr), elem.OID().toString());
            break;
        }
        case BSON_TYPE_BOOL:
            ASSERT_EQ(lbv.get()->value.v_bool, elem.Bool());
            break;
        case BSON_TYPE_DATE_TIME:
            ASSERT_EQ(lbv.get()->value.v_datetime, elem.Date().asInt64());
            break;
        case BSON_TYPE_NULL:
            break;
        case BSON_TYPE_REGEX:
            ASSERT_EQ(strcmp(lbv.get()->value.v_regex.regex, elem.regex()), 0);
            ASSERT_EQ(strcmp(lbv.get()->value.v_regex.options, elem.regexFlags()), 0);
            break;
        case BSON_TYPE_CODE:
            ASSERT_EQ(lbv.get()->value.v_code.code_len, elem._asCode().size());
            ASSERT_EQ(std::string(lbv.get()->value.v_code.code), elem._asCode());
            break;
        case BSON_TYPE_INT32:
            ASSERT_EQ(lbv.get()->value.v_int32, elem.Int());
            break;
        case BSON_TYPE_TIMESTAMP:
            ASSERT_EQ(lbv.get()->value.v_timestamp.timestamp, elem.timestamp().getSecs());
            ASSERT_EQ(lbv.get()->value.v_timestamp.increment, elem.timestamp().getInc());
            break;
        case BSON_TYPE_INT64:
            ASSERT_EQ(lbv.get()->value.v_int64, elem.Long());
            break;
        case BSON_TYPE_DECIMAL128:
            ASSERT_EQ(lbv.get()->value.v_decimal128.high, elem.Decimal().getValue().high64);
            ASSERT_EQ(lbv.get()->value.v_decimal128.low, elem.Decimal().getValue().low64);
            break;
        default:
            FAIL("Unknown BSON type");
    }
}

// Define macro to match other ASSERT_* macros
#define ASSERT_BSON_VALUE(lbv, type, elem) assertBSONValue(lbv, type, elem)


TEST(LibBSONValue, DefaultConstructor) {
    LibBSONValue lbv;
    ASSERT_EQ(lbv.get()->value_type, BSON_TYPE_EOD);
}

TEST(LibBSONValue, CopyFromBSONElement) {
    BSONObj obj = objectGenerator();

    BSONElement elem;

    LibBSONValue lbvEmpty;
    ASSERT_EQ(lbvEmpty.get()->value_type, BSON_TYPE_EOD);

    // Create a LibBSONValue for each element and assert correct initialization.
    for (const auto& pair : bsonTypeValueMap) {
        const std::string& fieldName = pair.first;
        const bson_type_t& type = pair.second.type;

        elem = obj[fieldName];
        LibBSONValue lbv(elem);
        ASSERT_BSON_VALUE(lbv, type, elem);
    }
}

TEST(LibBSONValue, CopyFromBsonValueT) {
    // Create bson_value_t.
    bson_value_t bvt;
    bvt.value_type = BSON_TYPE_DOUBLE;
    bvt.value.v_double = 3.14;

    // Copy to LibBSONValue.
    LibBSONValue lbv(bvt);
    ASSERT_EQ(lbv.get()->value_type, bvt.value_type);
    ASSERT_EQ(lbv.get()->value.v_double, bvt.value.v_double);
}

TEST(LibBSONValue, AssignmentOperator) {
    BSONObj obj = objectGenerator();
    LibBSONValue lbvFirst;
    LibBSONValue lbvSecond;

    // Continuously reassign lbvFirst and assert correct transfer of data.
    for (const auto& pair : bsonTypeValueMap) {
        const std::string& fieldName = pair.first;
        const bson_type_t& type = pair.second.type;

        BSONElement currElem = obj[fieldName];
        lbvSecond = LibBSONValue(currElem);
        lbvFirst = lbvSecond;
        ASSERT_BSON_VALUE(lbvFirst, type, currElem);
    }
}

TEST(LibBSONValue, CDRConstructor) {
    BSONObj obj = objectGenerator();

    // Create a ConstDataRange for each element, initalize a LibBSONValue off of the CDR, assert
    // correct construction.
    for (const auto& pair : bsonTypeValueMap) {
        const std::string& fieldName = pair.first;
        const bson_type_t& type = pair.second.type;

        BSONElement elem = obj[fieldName];
        ConstDataRange cdr(elem.value(), elem.value() + elem.valuesize());
        LibBSONValue lbv(findBSONTypeAlias(fieldName).get(), cdr);
        ASSERT_BSON_VALUE(lbv, type, elem);
    }
}

TEST(LibBSONValue, SerializeToBSONObjBuilder) {
    BSONObj obj = objectGenerator();
    BSONObjBuilder builder;

    // Serialize the entire object into a new object.
    for (auto& pair : bsonTypeValueMap) {
        std::string fieldName = pair.first;

        BSONElement elem = obj[fieldName];
        LibBSONValue lbv(elem);
        lbv.serialize(fieldName.append("Serialized"), &builder);
    }

    BSONObj objSerialized = builder.obj();

    // Check if the serialized object is the same as the original object (accounting for new field
    // names).
    for (auto& pair : bsonTypeValueMap) {
        std::string fieldName = pair.first;
        bson_type_t type = pair.second.type;

        BSONElement elem = obj[fieldName];
        BSONElement elemSerialized = objSerialized[fieldName.append("Serialized")];
        LibBSONValue lbv(elemSerialized);
        ASSERT_BSON_VALUE(lbv, type, elem);
    }
}

TEST(LibBSONValue, SerializeToBSONArrayBuilder) {
    BSONObj obj = objectGenerator();
    BSONArrayBuilder builder;

    // Serialize the entire object into an array.
    for (auto& pair : bsonTypeValueMap) {
        std::string fieldName = pair.first;

        BSONElement elem = obj[fieldName];
        LibBSONValue lbv(elem);
        lbv.serialize(&builder);
    }

    BSONArray arrSerialized = builder.arr();

    // Assert each value of the array.
    int index = 0;
    for (auto& pair : bsonTypeValueMap) {
        std::string fieldName = pair.first;
        bson_type_t type = pair.second.type;

        BSONElement elem = obj[fieldName];
        BSONElement elemSerialized = arrSerialized[index++];
        LibBSONValue lbv(elemSerialized);
        ASSERT_BSON_VALUE(lbv, type, elem);
    }
}

}  // namespace
}  // namespace mongo

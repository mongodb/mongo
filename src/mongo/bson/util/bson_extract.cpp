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

#include "mongo/bson/util/bson_extract.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

namespace {

Status bsonExtractFieldImpl(const BSONObj& object,
                            std::string_view fieldName,
                            BSONElement* outElement,
                            bool withDefault) {
    BSONElement element = object.getField(fieldName);

    if (!element.eoo()) {
        *outElement = element;
        return Status::OK();
    }
    if (withDefault) {
        static const Status kDefaultCase(ErrorCodes::NoSuchKey,
                                         "bsonExtractFieldImpl default case no such key error");
        return kDefaultCase;
    }
    return Status(ErrorCodes::NoSuchKey,
                  str::stream() << "Missing expected field \"" << std::string{fieldName} << "\"");
}

Status bsonExtractTypedFieldImpl(const BSONObj& object,
                                 std::string_view fieldName,
                                 BSONType type,
                                 BSONElement* outElement,
                                 bool withDefault) {
    Status status = bsonExtractFieldImpl(object, fieldName, outElement, withDefault);
    if (!status.isOK())
        return status;
    if (type != outElement->type()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "\"" << fieldName << "\" had the wrong type. Expected "
                          << typeName(type) << ", found " << typeName(outElement->type()));
    }
    return status;
}

Status bsonExtractIntegerFieldImpl(const BSONObj& object,
                                   std::string_view fieldName,
                                   long long* out,
                                   bool withDefault) {
    BSONElement element;
    Status status = bsonExtractFieldImpl(object, fieldName, &element, withDefault);
    if (!status.isOK())
        return status;
    if (!element.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "Expected field \"" << fieldName
                          << "\" to have numeric type, but found " << typeName(element.type()));
    }
    long long result = element.safeNumberLong();
    if (result != element.numberDouble()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Expected field \"" << fieldName
                                    << "\" to have a value "
                                       "exactly representable as a 64-bit integer, but found "
                                    << element);
    }
    *out = result;
    return status;
}

Status bsonExtractDoubleFieldImpl(const BSONObj& object,
                                  std::string_view fieldName,
                                  double* out,
                                  bool withDefault) {
    BSONElement element;
    Status status = bsonExtractField(object, fieldName, &element);
    if (!status.isOK())
        return status;
    if (!element.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "Expected field \"" << fieldName
                          << "\" to have numeric type, but found " << typeName(element.type()));
    }
    *out = element.numberDouble();
    return status;
}
}  // namespace


Status bsonExtractField(const BSONObj& object,
                        std::string_view fieldName,
                        BSONElement* outElement) {
    return bsonExtractFieldImpl(object, fieldName, outElement, false);
}

Status bsonExtractTypedField(const BSONObj& object,
                             std::string_view fieldName,
                             BSONType type,
                             BSONElement* outElement) {
    return bsonExtractTypedFieldImpl(object, fieldName, type, outElement, false);
}

Status bsonExtractBooleanField(const BSONObj& object, std::string_view fieldName, bool* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, BSONType::boolean, &element);
    if (status.isOK())
        *out = element.boolean();
    return status;
}

Status bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                          std::string_view fieldName,
                                          bool defaultValue,
                                          bool* out) {
    BSONElement element;
    Status status = bsonExtractFieldImpl(object, fieldName, &element, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue;
        return Status::OK();
    }

    if (!status.isOK())
        return status;

    if (!element.isNumber() && !element.isBoolean()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected boolean or number type for field \"" << fieldName
                                    << "\", found " << typeName(element.type()));
    }
    *out = element.trueValue();
    return status;
}

Status bsonExtractStringField(const BSONObj& object, std::string_view fieldName, std::string* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, BSONType::string, &element);
    if (status.isOK())
        *out = element.str();
    return status;
}

Status bsonExtractTimestampField(const BSONObj& object,
                                 std::string_view fieldName,
                                 Timestamp* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, BSONType::timestamp, &element);
    if (status.isOK())
        *out = element.timestamp();
    return status;
}

Status bsonExtractOIDField(const BSONObj& object, std::string_view fieldName, OID* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, BSONType::oid, &element);
    if (status.isOK())
        *out = element.OID();
    return status;
}

Status bsonExtractStringFieldWithDefault(const BSONObj& object,
                                         std::string_view fieldName,
                                         std::string_view defaultValue,
                                         std::string* out) {
    BSONElement element;
    Status status = bsonExtractTypedFieldImpl(object, fieldName, BSONType::string, &element, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = std::string{defaultValue};
        return Status::OK();
    }
    if (status.isOK())
        *out = element.str();
    return status;
}

Status bsonExtractIntegerField(const BSONObj& object, std::string_view fieldName, long long* out) {
    return bsonExtractIntegerFieldImpl(object, fieldName, out, false);
}

Status bsonExtractDoubleField(const BSONObj& object, std::string_view fieldName, double* out) {
    return bsonExtractDoubleFieldImpl(object, fieldName, out, false);
}

Status bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                          std::string_view fieldName,
                                          long long defaultValue,
                                          long long* out) {
    Status status = bsonExtractIntegerFieldImpl(object, fieldName, out, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue;
        return Status::OK();
    }
    return status;
}

////////////////////////////////////////////////////////////
// StatusWith variants of the above.

StatusWith<BSONElement> bsonExtractField(const BSONObj& object, std::string_view fieldName) {
    BSONElement out;
    if (auto st = bsonExtractField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<BSONElement> bsonExtractTypedField(const BSONObj& object,
                                              std::string_view fieldName,
                                              BSONType type) {
    BSONElement out;
    if (auto st = bsonExtractTypedField(object, fieldName, type, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<bool> bsonExtractBooleanField(const BSONObj& object, std::string_view fieldName) {
    bool out;
    if (auto st = bsonExtractBooleanField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<long long> bsonExtractIntegerField(const BSONObj& object, std::string_view fieldName) {
    long long out;
    if (auto st = bsonExtractIntegerField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<double> bsonExtractDoubleField(const BSONObj& object, std::string_view fieldName) {
    double out;
    if (auto st = bsonExtractDoubleField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<std::string> bsonExtractStringField(const BSONObj& object, std::string_view fieldName) {
    std::string out;
    if (auto st = bsonExtractStringField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<Timestamp> bsonExtractTimestampField(const BSONObj& object, std::string_view fieldName) {
    Timestamp out;
    if (auto st = bsonExtractTimestampField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<OID> bsonExtractOIDField(const BSONObj& object, std::string_view fieldName) {
    OID out;
    if (auto st = bsonExtractOIDField(object, fieldName, &out); !st.isOK())
        return st;
    return out;
}

StatusWith<bool> bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                                    std::string_view fieldName,
                                                    bool defaultValue) {
    bool out;
    if (auto st = bsonExtractBooleanFieldWithDefault(object, fieldName, defaultValue, &out);
        !st.isOK())
        return st;
    return out;
}

StatusWith<long long> bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                                         std::string_view fieldName,
                                                         long long defaultValue) {
    long long out;
    if (auto st = bsonExtractIntegerFieldWithDefault(object, fieldName, defaultValue, &out);
        !st.isOK())
        return st;
    return out;
}

StatusWith<std::string> bsonExtractStringFieldWithDefault(const BSONObj& object,
                                                          std::string_view fieldName,
                                                          std::string_view defaultValue) {
    std::string out;
    if (auto st = bsonExtractStringFieldWithDefault(object, fieldName, defaultValue, &out);
        !st.isOK())
        return st;
    return out;
}

}  // namespace mongo

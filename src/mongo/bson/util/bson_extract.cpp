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

namespace mongo {

namespace {

Status bsonExtractFieldImpl(const BSONObj& object,
                            StringData fieldName,
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
                  str::stream() << "Missing expected field \"" << fieldName.toString() << "\"");
}

Status bsonExtractTypedFieldImpl(const BSONObj& object,
                                 StringData fieldName,
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
                                   StringData fieldName,
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
                                  StringData fieldName,
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


Status bsonExtractField(const BSONObj& object, StringData fieldName, BSONElement* outElement) {
    return bsonExtractFieldImpl(object, fieldName, outElement, false);
}

Status bsonExtractTypedField(const BSONObj& object,
                             StringData fieldName,
                             BSONType type,
                             BSONElement* outElement) {
    return bsonExtractTypedFieldImpl(object, fieldName, type, outElement, false);
}

Status bsonExtractBooleanField(const BSONObj& object, StringData fieldName, bool* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, Bool, &element);
    if (status.isOK())
        *out = element.boolean();
    return status;
}

Status bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                          StringData fieldName,
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

Status bsonExtractStringField(const BSONObj& object, StringData fieldName, std::string* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, String, &element);
    if (status.isOK())
        *out = element.str();
    return status;
}

Status bsonExtractTimestampField(const BSONObj& object, StringData fieldName, Timestamp* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, bsonTimestamp, &element);
    if (status.isOK())
        *out = element.timestamp();
    return status;
}

Status bsonExtractOIDField(const BSONObj& object, StringData fieldName, OID* out) {
    BSONElement element;
    Status status = bsonExtractTypedField(object, fieldName, jstOID, &element);
    if (status.isOK())
        *out = element.OID();
    return status;
}

Status bsonExtractOIDFieldWithDefault(const BSONObj& object,
                                      StringData fieldName,
                                      const OID& defaultValue,
                                      OID* out) {
    BSONElement element;
    Status status = bsonExtractTypedFieldImpl(object, fieldName, jstOID, &element, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue;
        return Status::OK();
    }
    if (status.isOK())
        *out = element.OID();
    return status;
}

Status bsonExtractStringFieldWithDefault(const BSONObj& object,
                                         StringData fieldName,
                                         StringData defaultValue,
                                         std::string* out) {
    BSONElement element;
    Status status = bsonExtractTypedFieldImpl(object, fieldName, String, &element, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue.toString();
        return Status::OK();
    }
    if (status.isOK())
        *out = element.str();
    return status;
}

Status bsonExtractIntegerField(const BSONObj& object, StringData fieldName, long long* out) {
    return bsonExtractIntegerFieldImpl(object, fieldName, out, false);
}

Status bsonExtractDoubleField(const BSONObj& object, StringData fieldName, double* out) {
    return bsonExtractDoubleFieldImpl(object, fieldName, out, false);
}

Status bsonExtractDoubleFieldWithDefault(const BSONObj& object,
                                         StringData fieldName,
                                         double defaultValue,
                                         double* out) {
    Status status = bsonExtractDoubleFieldImpl(object, fieldName, out, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue;
        return Status::OK();
    }
    return status;
}

Status bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                          StringData fieldName,
                                          long long defaultValue,
                                          long long* out) {
    Status status = bsonExtractIntegerFieldImpl(object, fieldName, out, true);
    if (status == ErrorCodes::NoSuchKey) {
        *out = defaultValue;
        return Status::OK();
    }
    return status;
}

Status bsonExtractIntegerFieldWithDefaultIf(const BSONObj& object,
                                            StringData fieldName,
                                            long long defaultValue,
                                            std::function<bool(long long)> pred,
                                            const std::string& predDescription,
                                            long long* out) {
    Status status = bsonExtractIntegerFieldWithDefault(object, fieldName, defaultValue, out);
    if (!status.isOK()) {
        return status;
    }
    if (!pred(*out)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value in field \"" << fieldName << "\": " << *out
                                    << ": " << predDescription);
    }
    return status;
}

Status bsonExtractIntegerFieldWithDefaultIf(const BSONObj& object,
                                            StringData fieldName,
                                            long long defaultValue,
                                            std::function<bool(long long)> pred,
                                            long long* out) {
    return bsonExtractIntegerFieldWithDefaultIf(
        object, fieldName, defaultValue, pred, "constraint failed", out);
}

}  // namespace mongo

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/bson/util/bson_extract.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    Status bsonExtractField(const BSONObj& object,
                            const StringData& fieldName,
                            BSONElement* outElement) {
        BSONElement element = object.getField(fieldName);
        if (element.eoo())
            return Status(ErrorCodes::NoSuchKey,
                          mongoutils::str::stream() << "Missing expected field \"" <<
                                  fieldName.toString() << "\"");
        *outElement = element;
        return Status::OK();
    }

    Status bsonExtractTypedField(const BSONObj& object,
                                 const StringData& fieldName,
                                 BSONType type,
                                 BSONElement* outElement) {
        Status status = bsonExtractField(object, fieldName, outElement);
        if (!status.isOK())
            return status;
        if (type != outElement->type()) {
            return Status(ErrorCodes::TypeMismatch,
                          mongoutils::str::stream() << "\"" << fieldName <<
                          "\" had the wrong type. Expected " << typeName(type) <<
                          ", found " << typeName(outElement->type()));
        }
        return Status::OK();
    }

    Status bsonExtractBooleanField(const BSONObj& object,
                                   const StringData& fieldName,
                                   bool* out) {
        BSONElement element;
        Status status = bsonExtractTypedField(object, fieldName, Bool, &element);
        if (!status.isOK())
            return status;
        *out = element.boolean();
        return Status::OK();
    }

    Status bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                              const StringData& fieldName,
                                              bool defaultValue,
                                              bool* out) {
        BSONElement value;
        Status status = bsonExtractField(object, fieldName, &value);
        if (status == ErrorCodes::NoSuchKey) {
            *out = defaultValue;
            return Status::OK();
        }
        else if (!status.isOK()) {
            return status;
        }
        else if (!value.isNumber() && !value.isBoolean()) {
            return Status(ErrorCodes::TypeMismatch, mongoutils::str::stream() <<
                          "Expected boolean or number type for field \"" << fieldName <<
                          "\", found " << typeName(value.type()));
        }
        else {
            *out = value.trueValue();
            return Status::OK();
        }
    }

    Status bsonExtractStringField(const BSONObj& object,
                                  const StringData& fieldName,
                                  std::string* out) {
        BSONElement element;
        Status status = bsonExtractTypedField(object, fieldName, String, &element);
        if (!status.isOK())
            return status;
        *out = element.str();
        return Status::OK();
    }

    Status bsonExtractStringFieldWithDefault(const BSONObj& object,
                                             const StringData& fieldName,
                                             const StringData& defaultValue,
                                             std::string* out) {
        Status status = bsonExtractStringField(object, fieldName, out);
        if (status == ErrorCodes::NoSuchKey) {
            *out = defaultValue.toString();
        }
        else if (!status.isOK()) {
            return status;
        }
        return Status::OK();
    }

    Status bsonExtractIntegerField(const BSONObj& object,
                                   const StringData& fieldName,
                                   long long* out) {
        BSONElement value;
        Status status = bsonExtractField(object, fieldName, &value);
        if (!status.isOK())
            return status;
        if (!value.isNumber()) {
            return Status(ErrorCodes::TypeMismatch, mongoutils::str::stream() <<
                          "Expected field \"" << fieldName <<
                          "\" to have numeric type, but found " << typeName(value.type()));
        }
        long long result = value.safeNumberLong();
        if (result != value.numberDouble()) {
            return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                          "Expected field \"" << fieldName << "\" to have a value "
                          "exactly representable as a 64-bit integer, but found " <<
                          value);
        }
        *out = result;
        return Status::OK();
    }

    Status bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                              const StringData& fieldName,
                                              long long defaultValue,
                                              long long* out) {
        Status status = bsonExtractIntegerField(object, fieldName, out);
        if (status == ErrorCodes::NoSuchKey) {
            *out = defaultValue;
            status = Status::OK();
        }
        return status;
    }

}  // namespace mongo

/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/util/options_parser/value.h"

#include "mongo/bson/util/builder.h"

namespace mongo {
namespace optionenvironment {

    // Value implementation

    // Value access functions

    Status Value::get(std::vector<std::string>* val) const {
        if (type != StringVector) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: stringVector");
        }
        *val = stringVectorVal;
        return Status::OK();
    }
    Status Value::get(bool* val) const {
        if (type != Bool) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: bool");
        }
        *val = boolVal;
        return Status::OK();
    }
    Status Value::get(double* val) const {
        if (type != Double) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: double");
        }
        *val = doubleVal;
        return Status::OK();
    }
    Status Value::get(int* val) const {
        if (type != Int) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: int");
        }
        *val = intVal;
        return Status::OK();
    }
    Status Value::get(long* val) const {
        if (type == Long) {
            *val = longVal;
            return Status::OK();
        }
        else if (type == Int) {
            *val = intVal;
            return Status::OK();
        }
        return Status(ErrorCodes::TypeMismatch, "Value not convertible to type: long");
    }
    Status Value::get(std::string* val) const {
        if (type != String) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: string");
        }
        *val = stringVal;
        return Status::OK();
    }
    Status Value::get(unsigned long long* val) const {
        if (type == UnsignedLongLong) {
            *val = unsignedLongLongVal;
            return Status::OK();
        }
        else if (type == Unsigned) {
            *val = unsignedVal;
            return Status::OK();
        }
        return Status(ErrorCodes::TypeMismatch, "Value not convertible to type: unsignedlonglong");
    }
    Status Value::get(unsigned* val) const {
        if (type != Unsigned) {
            return Status(ErrorCodes::TypeMismatch, "Value not of type: unsigned");
        }
        *val = unsignedVal;
        return Status::OK();
    }

    // Value utility functions

    std::string Value::typeToString() const {
        switch (type) {
            case StringVector: return "StringVector";
            case Bool: return "Bool";
            case Double: return "Double";
            case Int: return "Int";
            case Long: return "Long";
            case String: return "String";
            case UnsignedLongLong: return "UnsignedLongLong";
            case Unsigned: return "Unsigned";
            case None: return "None";
            default: return "Unknown";
        }
    }
    bool Value::isEmpty() const {
        return type == None;
    }
    bool Value::equal(Value& otherVal) const {
        if (type != otherVal.type) {
            return false;
        }
        switch (type) {
            case StringVector: return stringVectorVal == otherVal.stringVectorVal;
            case Bool: return boolVal == otherVal.boolVal;
            case Double: return doubleVal == otherVal.doubleVal;
            case Int: return intVal == otherVal.intVal;
            case Long: return longVal == otherVal.longVal;
            case String: return stringVal == otherVal.stringVal;
            case UnsignedLongLong: return unsignedLongLongVal == otherVal.unsignedLongLongVal;
            case Unsigned: return unsignedVal == otherVal.unsignedVal;
            case None: return true;
            default: return false; /* Undefined */
        }
    }
    std::string Value::toString() const {
        StringBuilder sb;
        switch (type) {
            case StringVector:
                if (!stringVectorVal.empty())
                {
                    // Convert all but the last element to avoid a trailing ","
                    for(std::vector<std::string>::const_iterator iterator = stringVectorVal.begin();
                        iterator != stringVectorVal.end() - 1; iterator++) {
                        sb << *iterator;
                    }

                    // Now add the last element with no delimiter
                    sb << stringVectorVal.back();
                }
                break;
            case Bool: sb << boolVal; break;
            case Double: sb << doubleVal; break;
            case Int: sb << intVal; break;
            case Long: sb << longVal; break;
            case String: sb << stringVal; break;
            case UnsignedLongLong: sb << unsignedLongLongVal; break;
            case Unsigned: sb << unsignedVal; break;
            case None: sb << "(not set)"; break;
            default: sb << "(undefined)"; break;
        }
        return sb.str();
    }

} // namespace optionenvironment
} // namespace mongo

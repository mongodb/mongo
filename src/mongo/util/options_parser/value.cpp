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
        if (_type != StringVector) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: StringVector, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _stringVectorVal;
        return Status::OK();
    }
    Status Value::get(bool* val) const {
        if (_type != Bool) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: Bool, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _boolVal;
        return Status::OK();
    }
    Status Value::get(double* val) const {
        if (_type != Double) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: Double, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _doubleVal;
        return Status::OK();
    }
    Status Value::get(int* val) const {
        if (_type != Int) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: Int, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _intVal;
        return Status::OK();
    }
    Status Value::get(long* val) const {
        if (_type == Long) {
            *val = _longVal;
            return Status::OK();
        }
        else if (_type == Int) {
            *val = _intVal;
            return Status::OK();
        }
        StringBuilder sb;
        sb << "Value of type: " << typeToString()
           << " is not convertible to type: Long";
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    Status Value::get(std::string* val) const {
        if (_type != String) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: string, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _stringVal;
        return Status::OK();
    }
    Status Value::get(unsigned long long* val) const {
        if (_type == UnsignedLongLong) {
            *val = _unsignedLongLongVal;
            return Status::OK();
        }
        else if (_type == Unsigned) {
            *val = _unsignedVal;
            return Status::OK();
        }
        StringBuilder sb;
        sb << "Value of type: " << typeToString()
           << " is not convertible to type: UnsignedLongLong";
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    Status Value::get(unsigned* val) const {
        if (_type != Unsigned) {
            StringBuilder sb;
            sb << "Attempting to get Value as type: Unsigned, but Value is of type: "
               << typeToString();
            return Status(ErrorCodes::TypeMismatch, sb.str());
        }
        *val = _unsignedVal;
        return Status::OK();
    }

    // Value utility functions

    std::string Value::typeToString() const {
        switch (_type) {
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
        return _type == None;
    }
    bool Value::equal(Value& otherVal) const {
        if (_type != otherVal._type) {
            return false;
        }
        switch (_type) {
            case StringVector: return _stringVectorVal == otherVal._stringVectorVal;
            case Bool: return _boolVal == otherVal._boolVal;
            case Double: return _doubleVal == otherVal._doubleVal;
            case Int: return _intVal == otherVal._intVal;
            case Long: return _longVal == otherVal._longVal;
            case String: return _stringVal == otherVal._stringVal;
            case UnsignedLongLong: return _unsignedLongLongVal == otherVal._unsignedLongLongVal;
            case Unsigned: return _unsignedVal == otherVal._unsignedVal;
            case None: return true;
            default: return false; /* Undefined */
        }
    }
    std::string Value::toString() const {
        StringBuilder sb;
        switch (_type) {
            case StringVector:
                if (!_stringVectorVal.empty())
                {
                    // Convert all but the last element to avoid a trailing ","
                    for(std::vector<std::string>::const_iterator iterator = _stringVectorVal.begin();
                        iterator != _stringVectorVal.end() - 1; iterator++) {
                        sb << *iterator;
                    }

                    // Now add the last element with no delimiter
                    sb << _stringVectorVal.back();
                }
                break;
            case Bool: sb << _boolVal; break;
            case Double: sb << _doubleVal; break;
            case Int: sb << _intVal; break;
            case Long: sb << _longVal; break;
            case String: sb << _stringVal; break;
            case UnsignedLongLong: sb << _unsignedLongLongVal; break;
            case Unsigned: sb << _unsignedVal; break;
            case None: sb << "(not set)"; break;
            default: sb << "(undefined)"; break;
        }
        return sb.str();
    }
    const std::type_info& Value::type() const {
        switch (_type) {
            case StringVector: return typeid(std::vector<std::string>);
            case Bool: return typeid(bool);
            case Double: return typeid(double);
            case Int: return typeid(int);
            case Long: return typeid(long);
            case String: return typeid(std::string);
            case UnsignedLongLong: return typeid(unsigned long long);
            case Unsigned: return typeid(unsigned);
            case None: return typeid(void);
            default: return typeid(void);
        }
    }

} // namespace optionenvironment
} // namespace mongo

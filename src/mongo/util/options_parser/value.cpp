/* Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/util/options_parser/value.h"

#include "mongo/bson/util/builder.h"

namespace mongo {
namespace optionenvironment {

// Value implementation

// Value access functions

Status Value::get(StringVector_t* val) const {
    if (_type != StringVector) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: StringVector, but Value is of type: "
           << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _stringVectorVal;
    return Status::OK();
}
Status Value::get(StringMap_t* val) const {
    if (_type != StringMap) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: StringMap, but Value is of type: "
           << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _stringMapVal;
    return Status::OK();
}
Status Value::get(bool* val) const {
    if (_type != Bool) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: Bool, but Value is of type: " << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _boolVal;
    return Status::OK();
}
Status Value::get(double* val) const {
    if (_type != Double) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: Double, but Value is of type: " << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _doubleVal;
    return Status::OK();
}
Status Value::get(int* val) const {
    if (_type != Int) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: Int, but Value is of type: " << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _intVal;
    return Status::OK();
}
Status Value::get(long* val) const {
    if (_type == Long) {
        *val = _longVal;
        return Status::OK();
    } else if (_type == Int) {
        *val = _intVal;
        return Status::OK();
    }
    StringBuilder sb;
    sb << "Value of type: " << typeToString() << " is not convertible to type: Long";
    return Status(ErrorCodes::TypeMismatch, sb.str());
}
Status Value::get(std::string* val) const {
    if (_type != String) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: string, but Value is of type: " << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _stringVal;
    return Status::OK();
}
Status Value::get(unsigned long long* val) const {
    if (_type == UnsignedLongLong) {
        *val = _unsignedLongLongVal;
        return Status::OK();
    } else if (_type == Unsigned) {
        *val = _unsignedVal;
        return Status::OK();
    }
    StringBuilder sb;
    sb << "Value of type: " << typeToString() << " is not convertible to type: UnsignedLongLong";
    return Status(ErrorCodes::TypeMismatch, sb.str());
}
Status Value::get(unsigned* val) const {
    if (_type != Unsigned) {
        StringBuilder sb;
        sb << "Attempting to get Value as type: Unsigned, but Value is of type: " << typeToString();
        return Status(ErrorCodes::TypeMismatch, sb.str());
    }
    *val = _unsignedVal;
    return Status::OK();
}

// Value utility functions

std::string Value::typeToString() const {
    switch (_type) {
        case StringVector:
            return "StringVector";
        case StringMap:
            return "StringMap";
        case Bool:
            return "Bool";
        case Double:
            return "Double";
        case Int:
            return "Int";
        case Long:
            return "Long";
        case String:
            return "String";
        case UnsignedLongLong:
            return "UnsignedLongLong";
        case Unsigned:
            return "Unsigned";
        case None:
            return "None";
        default:
            return "Unknown";
    }
}
bool Value::isEmpty() const {
    return _type == None;
}
bool Value::equal(const Value& otherVal) const {
    if (_type != otherVal._type) {
        return false;
    }
    switch (_type) {
        case StringVector:
            return _stringVectorVal == otherVal._stringVectorVal;
        case StringMap:
            return _stringMapVal == otherVal._stringMapVal;
        case Bool:
            return _boolVal == otherVal._boolVal;
        case Double:
            return _doubleVal == otherVal._doubleVal;
        case Int:
            return _intVal == otherVal._intVal;
        case Long:
            return _longVal == otherVal._longVal;
        case String:
            return _stringVal == otherVal._stringVal;
        case UnsignedLongLong:
            return _unsignedLongLongVal == otherVal._unsignedLongLongVal;
        case Unsigned:
            return _unsignedVal == otherVal._unsignedVal;
        case None:
            return true;
        default:
            return false; /* Undefined */
    }
}

// Dump the value as a string.  This function is used only for debugging purposes.
std::string Value::toString() const {
    StringBuilder sb;
    switch (_type) {
        case StringVector:
            if (!_stringVectorVal.empty()) {
                // Convert all but the last element to avoid a trailing ","
                for (StringVector_t::const_iterator iterator = _stringVectorVal.begin();
                     iterator != _stringVectorVal.end() - 1;
                     iterator++) {
                    sb << *iterator << ",";
                }

                // Now add the last element with no delimiter
                sb << _stringVectorVal.back();
            }
            break;
        case StringMap:
            if (!_stringMapVal.empty()) {
                // Convert all but the last element to avoid a trailing ","
                if (_stringMapVal.begin() != _stringMapVal.end()) {
                    StringMap_t::const_iterator iterator;
                    StringMap_t::const_iterator it_last;
                    for (iterator = _stringMapVal.begin(), it_last = --_stringMapVal.end();
                         iterator != it_last;
                         ++iterator) {
                        sb << iterator->first << ":" << iterator->second << ",";
                    }
                }

                // Now add the last element with no delimiter
                sb << _stringMapVal.end()->first << ":" << _stringMapVal.end()->second;
            }
            break;
        case Bool:
            sb << _boolVal;
            break;
        case Double:
            sb << _doubleVal;
            break;
        case Int:
            sb << _intVal;
            break;
        case Long:
            sb << _longVal;
            break;
        case String:
            sb << _stringVal;
            break;
        case UnsignedLongLong:
            sb << _unsignedLongLongVal;
            break;
        case Unsigned:
            sb << _unsignedVal;
            break;
        case None:
            sb << "(not set)";
            break;
        default:
            sb << "(undefined)";
            break;
    }
    return sb.str();
}
const std::type_info& Value::type() const {
    switch (_type) {
        case StringVector:
            return typeid(StringVector_t);
        case StringMap:
            return typeid(StringMap_t);
        case Bool:
            return typeid(bool);
        case Double:
            return typeid(double);
        case Int:
            return typeid(int);
        case Long:
            return typeid(long);
        case String:
            return typeid(std::string);
        case UnsignedLongLong:
            return typeid(unsigned long long);
        case Unsigned:
            return typeid(unsigned);
        case None:
            return typeid(void);
        default:
            return typeid(void);
    }
}

}  // namespace optionenvironment
}  // namespace mongo

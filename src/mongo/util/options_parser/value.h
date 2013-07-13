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

#pragma once

#include <vector>

#include "mongo/base/status.h"

namespace mongo {
namespace optionenvironment {

    class Constraint;
    class KeyConstraint;

    typedef std::string Key;

    /** A simple container interface for storing various C++ values.
     *
     *  Usage:
     *
     *  Value intVal(2);
     *  Value stringVal("string");
     *
     *  int intContents = 1;
     *  Status ret = stringVal.get(&intContents);
     *  // ret != Status::OK()
     *  // intContents is still 1
     *
     *  ret = intVal.get(&intContents);
     *  // ret == Status::OK()
     *  // intContents is now 2
     */
    class Value {
    public:

        // Constructors

        explicit Value() : type(None) { }
        explicit Value(std::vector<std::string> val) : stringVectorVal(val), type(StringVector) { }
        explicit Value(bool val) : boolVal(val), type(Bool) { }
        explicit Value(double val) : doubleVal(val), type(Double) { }
        explicit Value(int val) : intVal(val), type(Int) { }
        explicit Value(long val) : longVal(val), type(Long) { }
        explicit Value(std::string val) : stringVal(val), type(String) { }
        explicit Value(unsigned long long val) : unsignedLongLongVal(val), type(UnsignedLongLong) {}
        explicit Value(unsigned val) : unsignedVal(val), type(Unsigned) { }

        // Access interface

        Status get(std::vector<std::string>* val) const;
        Status get(bool* val) const;
        Status get(double* val) const;
        Status get(int* val) const;
        Status get(long* val) const;
        Status get(string* val) const;
        Status get(unsigned long long* val) const;
        Status get(unsigned* val) const;

        // Utility functions

        /**
         *  Return the value's type as a string
         */
        std::string typeToString() const;

        /**
         *  Return true if the value was created with the no argument constructor
         */
        bool isEmpty() const;

        /**
         *  Return true if the other Value equals this value, both in type and in contents
         *
         *  Two empty values are equal
         */
        bool equal(Value&) const;

        /**
         *  Return the string representation of this Value
         */
        std::string toString() const;

    private:
        std::vector<std::string> stringVectorVal;
        std::string stringVal;
        union {
            bool boolVal;
            double doubleVal;
            int intVal;
            long longVal;
            unsigned long long unsignedLongLongVal;
            unsigned unsignedVal;
        };

        // Types currently supported by Value
        enum Type {
            StringVector,     // std::vector<std::string>
            Bool,             // bool
            Double,           // double
            Int,              // int
            Long,             // long
            String,           // std::string
            UnsignedLongLong, // unsigned long long
            Unsigned,         // unsigned
            None,             // (not set)
        };

        Type type;
    };

} // namespace optionenvironment
} // namespace mongo

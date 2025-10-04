/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_LogicalType_hh__
#define avro_LogicalType_hh__

#include <iostream>

#include "Config.hh"

namespace avro {

class AVRO_DECL LogicalType {
public:
    enum Type {
        NONE,
        DECIMAL,
        DATE,
        TIME_MILLIS,
        TIME_MICROS,
        TIMESTAMP_MILLIS,
        TIMESTAMP_MICROS,
        DURATION,
        UUID
    };

    explicit LogicalType(Type type);

    Type type() const;

    // Precision and scale can only be set for the DECIMAL logical type.
    // Precision must be positive and scale must be either positive or zero. The
    // setters will throw an exception if they are called on any type other
    // than DECIMAL.
    void setPrecision(int32_t precision);
    int32_t precision() const { return precision_; }
    void setScale(int32_t scale);
    int32_t scale() const { return scale_; }

    void printJson(std::ostream &os) const;

private:
    Type type_;
    int32_t precision_;
    int32_t scale_;
};

} // namespace avro

#endif

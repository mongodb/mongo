/**
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

#include "GenericDatum.hh"
#include "NodeImpl.hh"

using std::string;
using std::vector;

namespace avro {

GenericDatum::GenericDatum(const ValidSchema &schema) : type_(schema.root()->type()),
                                                        logicalType_(schema.root()->logicalType()) {
    init(schema.root());
}

GenericDatum::GenericDatum(const NodePtr &schema) : type_(schema->type()),
                                                    logicalType_(schema->logicalType()) {
    init(schema);
}

void GenericDatum::init(const NodePtr &schema) {
    NodePtr sc = schema;
    if (type_ == AVRO_SYMBOLIC) {
        sc = resolveSymbol(schema);
        type_ = sc->type();
        logicalType_ = sc->logicalType();
    }
    switch (type_) {
        case AVRO_NULL: break;
        case AVRO_BOOL:
            value_ = bool();
            break;
        case AVRO_INT:
            value_ = int32_t();
            break;
        case AVRO_LONG:
            value_ = int64_t();
            break;
        case AVRO_FLOAT:
            value_ = float();
            break;
        case AVRO_DOUBLE:
            value_ = double();
            break;
        case AVRO_STRING:
            value_ = string();
            break;
        case AVRO_BYTES:
            value_ = vector<uint8_t>();
            break;
        case AVRO_FIXED:
            value_ = GenericFixed(sc);
            break;
        case AVRO_RECORD:
            value_ = GenericRecord(sc);
            break;
        case AVRO_ENUM:
            value_ = GenericEnum(sc);
            break;
        case AVRO_ARRAY:
            value_ = GenericArray(sc);
            break;
        case AVRO_MAP:
            value_ = GenericMap(sc);
            break;
        case AVRO_UNION:
            value_ = GenericUnion(sc);
            break;
        default:
            throw Exception("Unknown schema type {}", toString(type_));
    }
}

GenericRecord::GenericRecord(const NodePtr &schema) : GenericContainer(AVRO_RECORD, schema) {
    fields_.resize(schema->leaves());
    for (size_t i = 0; i < schema->leaves(); ++i) {
        fields_[i] = GenericDatum(schema->leafAt(i));
    }
}

GenericFixed::GenericFixed(const NodePtr &schema, const vector<uint8_t> &v) : GenericContainer(AVRO_FIXED, schema), value_(v) {}
} // namespace avro

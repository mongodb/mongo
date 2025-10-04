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

#ifndef avro_Schema_hh__
#define avro_Schema_hh__

#include "Config.hh"
#include "CustomAttributes.hh"
#include "NodeImpl.hh"
#include <string>

/// \file
///
/// Schemas for representing all the avro types.  The compound schema objects
/// allow composition from other schemas.
///

namespace avro {

/// The root Schema object is a base class.  Nobody constructs this class directly.

class AVRO_DECL Schema {
public:
    virtual ~Schema() = default;

    Type type() const {
        return node_->type();
    }

    const NodePtr &root() const {
        return node_;
    }

    NodePtr &root() {
        return node_;
    }

protected:
    explicit Schema(NodePtr node) : node_(std::move(node)) {}
    explicit Schema(Node *node) : node_(node) {}

    NodePtr node_;
};

class AVRO_DECL NullSchema : public Schema {
public:
    NullSchema() : Schema(new NodePrimitive(AVRO_NULL)) {}
};

class AVRO_DECL BoolSchema : public Schema {
public:
    BoolSchema() : Schema(new NodePrimitive(AVRO_BOOL)) {}
};

class AVRO_DECL IntSchema : public Schema {
public:
    IntSchema() : Schema(new NodePrimitive(AVRO_INT)) {}
};

class AVRO_DECL LongSchema : public Schema {
public:
    LongSchema() : Schema(new NodePrimitive(AVRO_LONG)) {}
};

class AVRO_DECL FloatSchema : public Schema {
public:
    FloatSchema() : Schema(new NodePrimitive(AVRO_FLOAT)) {}
};

class AVRO_DECL DoubleSchema : public Schema {
public:
    DoubleSchema() : Schema(new NodePrimitive(AVRO_DOUBLE)) {}
};

class AVRO_DECL StringSchema : public Schema {
public:
    StringSchema() : Schema(new NodePrimitive(AVRO_STRING)) {}
};

class AVRO_DECL BytesSchema : public Schema {
public:
    BytesSchema() : Schema(new NodePrimitive(AVRO_BYTES)) {}
};

class AVRO_DECL RecordSchema : public Schema {
public:
    explicit RecordSchema(const std::string &name);
    void addField(const std::string &name, const Schema &fieldSchema);
    // Add a field with custom attributes
    void addField(const std::string &name, const Schema &fieldSchema,
                  const CustomAttributes &customAttributes);

    std::string getDoc() const;
    void setDoc(const std::string &);
};

class AVRO_DECL EnumSchema : public Schema {
public:
    explicit EnumSchema(const std::string &name);
    void addSymbol(const std::string &symbol);
};

class AVRO_DECL ArraySchema : public Schema {
public:
    explicit ArraySchema(const Schema &itemsSchema);
    ArraySchema(const ArraySchema &itemsSchema);
};

class AVRO_DECL MapSchema : public Schema {
public:
    explicit MapSchema(const Schema &valuesSchema);
    MapSchema(const MapSchema &itemsSchema);
};

class AVRO_DECL UnionSchema : public Schema {
public:
    UnionSchema();
    void addType(const Schema &typeSchema);
};

class AVRO_DECL FixedSchema : public Schema {
public:
    FixedSchema(int size, const std::string &name);
};

class AVRO_DECL SymbolicSchema : public Schema {
public:
    SymbolicSchema(const Name &name, const NodePtr &link);
};
} // namespace avro

#endif

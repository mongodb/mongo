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

#ifndef avro_json_JsonDom_hh__
#define avro_json_JsonDom_hh__

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Config.hh"
#include "boost/any.hpp"

namespace avro {

class AVRO_DECL InputStream;

namespace json {
class Entity;

typedef bool Bool;
typedef int64_t Long;
typedef double Double;
typedef std::string String;
typedef std::vector<Entity> Array;
typedef std::map<std::string, Entity> Object;

class AVRO_DECL JsonParser;
class JsonNullFormatter;

template<typename F = JsonNullFormatter>
class AVRO_DECL JsonGenerator;

enum class EntityType {
    Null,
    Bool,
    Long,
    Double,
    String,
    Arr,
    Obj
};

const char *typeToString(EntityType t);

inline std::ostream &operator<<(std::ostream &os, EntityType et) {
    return os << typeToString(et);
}

class AVRO_DECL Entity {
    EntityType type_;
    boost::any value_;
    size_t line_; // can't be const else noncopyable...

    void ensureType(EntityType) const;

public:
    explicit Entity(size_t line = 0) : type_(EntityType::Null), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(Bool v, size_t line = 0) : type_(EntityType::Bool), value_(v), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(Long v, size_t line = 0) : type_(EntityType::Long), value_(v), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(Double v, size_t line = 0) : type_(EntityType::Double), value_(v), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(const std::shared_ptr<String> &v, size_t line = 0) : type_(EntityType::String), value_(v), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(const std::shared_ptr<Array> &v, size_t line = 0) : type_(EntityType::Arr), value_(v), line_(line) {}
    // Not explicit because do want implicit conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    explicit Entity(const std::shared_ptr<Object> &v, size_t line = 0) : type_(EntityType::Obj), value_(v), line_(line) {}

    EntityType type() const { return type_; }

    size_t line() const { return line_; }

    Bool boolValue() const {
        ensureType(EntityType::Bool);
        return boost::any_cast<Bool>(value_);
    }

    Long longValue() const {
        ensureType(EntityType::Long);
        return boost::any_cast<Long>(value_);
    }

    Double doubleValue() const {
        ensureType(EntityType::Double);
        return boost::any_cast<Double>(value_);
    }

    String stringValue() const;

    String bytesValue() const;

    const Array &arrayValue() const {
        ensureType(EntityType::Arr);
        return **boost::any_cast<std::shared_ptr<Array>>(&value_);
    }

    const Object &objectValue() const {
        ensureType(EntityType::Obj);
        return **boost::any_cast<std::shared_ptr<Object>>(&value_);
    }

    std::string toString() const;
};

template<typename T>
struct type_traits {
};

template<>
struct type_traits<bool> {
    static EntityType type() { return EntityType::Bool; }
    static const char *name() { return "bool"; }
};

template<>
struct type_traits<int64_t> {
    static EntityType type() { return EntityType::Long; }
    static const char *name() { return "long"; }
};

template<>
struct type_traits<double> {
    static EntityType type() { return EntityType::Double; }
    static const char *name() { return "double"; }
};

template<>
struct type_traits<std::string> {
    static EntityType type() { return EntityType::String; }
    static const char *name() { return "string"; }
};

template<>
struct type_traits<std::vector<Entity>> {
    static EntityType type() { return EntityType::Arr; }
    static const char *name() { return "array"; }
};

template<>
struct type_traits<std::map<std::string, Entity>> {
    static EntityType type() { return EntityType::Obj; }
    static const char *name() { return "object"; }
};

AVRO_DECL Entity readEntity(JsonParser &p);

AVRO_DECL Entity loadEntity(InputStream &in);
AVRO_DECL Entity loadEntity(const char *text);
AVRO_DECL Entity loadEntity(const uint8_t *text, size_t len);

void writeEntity(JsonGenerator<JsonNullFormatter> &g, const Entity &n);

} // namespace json
} // namespace avro

#endif

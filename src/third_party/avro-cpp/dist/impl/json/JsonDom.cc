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

#include "JsonDom.hh"

#include <stdexcept>

#include <cstring>

#include "JsonIO.hh"
#include "Stream.hh"

namespace avro {
namespace json {
const char *typeToString(EntityType t) {
    switch (t) {
        case EntityType::Null: return "null";
        case EntityType::Bool: return "bool";
        case EntityType::Long: return "long";
        case EntityType::Double: return "double";
        case EntityType::String: return "string";
        case EntityType::Arr: return "array";
        case EntityType::Obj: return "object";
        default: return "unknown";
    }
}

Entity readEntity(JsonParser &p) {
    switch (p.peek()) {
        case JsonParser::Token::Null:
            p.advance();
            return Entity(p.line());
        case JsonParser::Token::Bool:
            p.advance();
            return Entity(p.boolValue(), p.line());
        case JsonParser::Token::Long:
            p.advance();
            return Entity(p.longValue(), p.line());
        case JsonParser::Token::Double:
            p.advance();
            return Entity(p.doubleValue(), p.line());
        case JsonParser::Token::String:
            p.advance();
            return Entity(std::make_shared<String>(p.rawString()), p.line());
        case JsonParser::Token::ArrayStart: {
            size_t l = p.line();
            p.advance();
            std::shared_ptr<Array> v = std::make_shared<Array>();
            while (p.peek() != JsonParser::Token::ArrayEnd) {
                v->push_back(readEntity(p));
            }
            p.advance();
            return Entity(v, l);
        }
        case JsonParser::Token::ObjectStart: {
            size_t l = p.line();
            p.advance();
            std::shared_ptr<Object> v = std::make_shared<Object>();
            while (p.peek() != JsonParser::Token::ObjectEnd) {
                p.advance();
                std::string k = p.stringValue();
                Entity n = readEntity(p);
                v->insert(std::make_pair(k, n));
            }
            p.advance();
            return Entity(v, l);
        }
        default:
            throw std::domain_error(JsonParser::toString(p.peek()));
    }
}

Entity loadEntity(const char *text) {
    return loadEntity(reinterpret_cast<const uint8_t *>(text), ::strlen(text));
}

Entity loadEntity(InputStream &in) {
    JsonParser p;
    p.init(in);
    return readEntity(p);
}

Entity loadEntity(const uint8_t *text, size_t len) {
    std::unique_ptr<InputStream> in = memoryInputStream(text, len);
    return loadEntity(*in);
}

void writeEntity(JsonGenerator<JsonNullFormatter> &g, const Entity &n) {
    switch (n.type()) {
        case EntityType::Null:
            g.encodeNull();
            break;
        case EntityType::Bool:
            g.encodeBool(n.boolValue());
            break;
        case EntityType::Long:
            g.encodeNumber(n.longValue());
            break;
        case EntityType::Double:
            g.encodeNumber(n.doubleValue());
            break;
        case EntityType::String:
            g.encodeString(n.stringValue());
            break;
        case EntityType::Arr: {
            g.arrayStart();
            const Array &v = n.arrayValue();
            for (const auto &it : v) {
                writeEntity(g, it);
            }
            g.arrayEnd();
        } break;
        case EntityType::Obj: {
            g.objectStart();
            const Object &v = n.objectValue();
            for (const auto &it : v) {
                g.encodeString(it.first);
                writeEntity(g, it.second);
            }
            g.objectEnd();
        } break;
    }
}

void Entity::ensureType(EntityType type) const {
    if (type_ != type) {
        throw Exception("Invalid type. Expected \"{}\" actual {}", typeToString(type), typeToString(type_));
    }
}

String Entity::stringValue() const {
    ensureType(EntityType::String);
    return JsonParser::toStringValue(**boost::any_cast<std::shared_ptr<String>>(&value_));
}

String Entity::bytesValue() const {
    ensureType(EntityType::String);
    return JsonParser::toBytesValue(**boost::any_cast<std::shared_ptr<String>>(&value_));
}

std::string Entity::toString() const {
    std::unique_ptr<OutputStream> out = memoryOutputStream();
    JsonGenerator<JsonNullFormatter> g;
    g.init(*out);
    writeEntity(g, *this);
    g.flush();
    std::unique_ptr<InputStream> in = memoryInputStream(*out);
    const uint8_t *p = nullptr;
    size_t n = 0;
    size_t c = 0;
    while (in->next(&p, &n)) {
        c += n;
    }
    std::string result;
    result.resize(c);
    c = 0;
    std::unique_ptr<InputStream> in2 = memoryInputStream(*out);
    while (in2->next(&p, &n)) {
        ::memcpy(&result[c], p, n);
        c += n;
    }
    return result;
}

} // namespace json
} // namespace avro

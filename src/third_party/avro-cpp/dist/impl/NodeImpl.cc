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

#include "NodeImpl.hh"
#include <sstream>
#include <utility>

using std::string;
namespace avro {

namespace {

// Escape string for serialization.
string escape(const string &unescaped) {
    string s;
    s.reserve(unescaped.length());
    for (char c : unescaped) {
        switch (c) {
            case '\\':
            case '"':
            case '/':
                s += '\\';
                s += c;
                break;
            case '\b':
                s += '\\';
                s += 'b';
                break;
            case '\f':
                s += '\f';
                break;
            case '\n':
                s += '\\';
                s += 'n';
                break;
            case '\r':
                s += '\\';
                s += 'r';
                break;
            case '\t':
                s += '\\';
                s += 't';
                break;
            default:
                if (!std::iscntrl(c, std::locale::classic())) {
                    s += c;
                    continue;
                }
                s += intToHex(static_cast<unsigned int>(c));
                break;
        }
    }
    return s;
}

// Wrap an indentation in a struct for ostream operator<<
struct indent {
    explicit indent(size_t depth) : d(depth) {}
    size_t d;
};

/// ostream operator for indent
std::ostream &operator<<(std::ostream &os, indent x) {
    static const string spaces("    ");
    while (x.d--) {
        os << spaces;
    }
    return os;
}

void printCustomAttributes(const CustomAttributes &customAttributes, size_t depth,
                           std::ostream &os) {
    std::map<std::string, std::string>::const_iterator iter =
        customAttributes.attributes().begin();
    while (iter != customAttributes.attributes().end()) {
        os << ",\n"
           << indent(depth);
        customAttributes.printJson(os, iter->first);
        ++iter;
    }
}

} // anonymous namespace

const int kByteStringSize = 6;

SchemaResolution
NodePrimitive::resolve(const Node &reader) const {
    if (type() == reader.type()) {
        return RESOLVE_MATCH;
    }

    switch (type()) {

        case AVRO_INT:

            if (reader.type() == AVRO_LONG) {
                return RESOLVE_PROMOTABLE_TO_LONG;
            }

            [[fallthrough]];

        case AVRO_LONG:

            if (reader.type() == AVRO_FLOAT) {
                return RESOLVE_PROMOTABLE_TO_FLOAT;
            }

            [[fallthrough]];

        case AVRO_FLOAT:

            if (reader.type() == AVRO_DOUBLE) {
                return RESOLVE_PROMOTABLE_TO_DOUBLE;
            }

        default: break;
    }

    return furtherResolution(reader);
}

SchemaResolution
NodeRecord::resolve(const Node &reader) const {
    if (reader.type() == AVRO_RECORD) {
        if (name() == reader.name()) {
            return RESOLVE_MATCH;
        }
    }
    return furtherResolution(reader);
}

SchemaResolution
NodeEnum::resolve(const Node &reader) const {
    if (reader.type() == AVRO_ENUM) {
        return (name() == reader.name()) ? RESOLVE_MATCH : RESOLVE_NO_MATCH;
    }
    return furtherResolution(reader);
}

SchemaResolution
NodeArray::resolve(const Node &reader) const {
    if (reader.type() == AVRO_ARRAY) {
        const NodePtr &arrayType = leafAt(0);
        return arrayType->resolve(*reader.leafAt(0));
    }
    return furtherResolution(reader);
}

SchemaResolution
NodeMap::resolve(const Node &reader) const {
    if (reader.type() == AVRO_MAP) {
        const NodePtr &mapType = leafAt(1);
        return mapType->resolve(*reader.leafAt(1));
    }
    return furtherResolution(reader);
}

SchemaResolution
NodeUnion::resolve(const Node &reader) const {

    // If the writer is union, resolution only needs to occur when the selected
    // type of the writer is known, so this function is not very helpful.
    //
    // In this case, this function returns if there is a possible match given
    // any writer type, so just search type by type returning the best match
    // found.

    SchemaResolution match = RESOLVE_NO_MATCH;
    for (size_t i = 0; i < leaves(); ++i) {
        const NodePtr &node = leafAt(i);
        SchemaResolution thisMatch = node->resolve(reader);
        if (thisMatch == RESOLVE_MATCH) {
            match = thisMatch;
            break;
        }
        if (match == RESOLVE_NO_MATCH) {
            match = thisMatch;
        }
    }
    return match;
}

SchemaResolution
NodeFixed::resolve(const Node &reader) const {
    if (reader.type() == AVRO_FIXED) {
        return (
                   (reader.fixedSize() == fixedSize()) && (reader.name() == name()))
            ? RESOLVE_MATCH
            : RESOLVE_NO_MATCH;
    }
    return furtherResolution(reader);
}

SchemaResolution
NodeSymbolic::resolve(const Node &reader) const {
    const NodePtr &node = leafAt(0);
    return node->resolve(reader);
}

void NodePrimitive::printJson(std::ostream &os, size_t depth) const {
    bool hasLogicalType = logicalType().type() != LogicalType::NONE;

    if (hasLogicalType) {
        os << "{\n"
           << indent(depth) << "\"type\": ";
    }

    os << '\"' << type() << '\"';

    if (hasLogicalType) {
        os << ",\n"
           << indent(depth);
        logicalType().printJson(os);
        os << "\n}";
    }
    if (!getDoc().empty()) {
        os << ",\n"
           << indent(depth) << R"("doc": ")"
           << escape(getDoc()) << "\"";
    }
}

void NodeSymbolic::printJson(std::ostream &os, size_t depth) const {
    os << '\"' << nameAttribute_.get() << '\"';
    if (!getDoc().empty()) {
        os << ",\n"
           << indent(depth) << R"("doc": ")"
           << escape(getDoc()) << "\"";
    }
}

static void printName(std::ostream &os, const Name &n, size_t depth) {
    if (!n.ns().empty()) {
        os << indent(depth) << R"("namespace": ")" << n.ns() << "\",\n";
    }
    os << indent(depth) << R"("name": ")" << n.simpleName() << "\",\n";
}

void NodeRecord::printJson(std::ostream &os, size_t depth) const {
    os << "{\n";
    os << indent(++depth) << "\"type\": \"record\",\n";
    const Name &name = nameAttribute_.get();
    printName(os, name, depth);

    const auto &aliases = name.aliases();
    if (!aliases.empty()) {
        os << indent(depth) << "\"aliases\": [";
        ++depth;
        for (size_t i = 0; i < aliases.size(); ++i) {
            if (i > 0) {
                os << ',';
            }
            os << '\n'
               << indent(depth) << "\"" << aliases[i] << "\"";
        }
        os << '\n'
           << indent(--depth) << "]\n";
    }

    if (!getDoc().empty()) {
        os << indent(depth) << R"("doc": ")"
           << escape(getDoc()) << "\",\n";
    }

    os << indent(depth) << "\"fields\": [";
    size_t fields = leafAttributes_.size();
    ++depth;
    assert(fieldsAliases_.empty() || (fieldsAliases_.size() == fields));
    assert(fieldsDefaultValues_.empty() || (fieldsDefaultValues_.size() == fields));
    assert(customAttributes_.size() == 0 || customAttributes_.size() == fields);
    for (size_t i = 0; i < fields; ++i) {
        if (i > 0) {
            os << ',';
        }
        os << '\n'
           << indent(depth) << "{\n";
        os << indent(++depth) << R"("name": ")" << leafNameAttributes_.get(i) << "\",\n";
        os << indent(depth) << "\"type\": ";
        leafAttributes_.get(i)->printJson(os, depth);

        if (!fieldsAliases_.empty() && !fieldsAliases_[i].empty()) {
            os << ",\n"
               << indent(depth) << "\"aliases\": [";
            ++depth;
            for (size_t j = 0; j < fieldsAliases_[i].size(); ++j) {
                if (j > 0) {
                    os << ',';
                }
                os << '\n'
                   << indent(depth) << "\"" << fieldsAliases_[i][j] << "\"";
            }
            os << '\n'
               << indent(--depth) << ']';
        }

        // Serialize "default" field:
        if (!fieldsDefaultValues_.empty()) {
            if (!fieldsDefaultValues_[i].isUnion() && fieldsDefaultValues_[i].type() == AVRO_NULL) {
                // No "default" field.
            } else {
                os << ",\n"
                   << indent(depth) << "\"default\": ";
                leafAttributes_.get(i)->printDefaultToJson(fieldsDefaultValues_[i], os,
                                                           depth);
            }
        }

        if (customAttributes_.size() == fields) {
            printCustomAttributes(customAttributes_.get(i), depth, os);
        }

        os << '\n';
        os << indent(--depth) << '}';
    }
    os << '\n'
       << indent(--depth) << "]\n";
    os << indent(--depth) << '}';
}

void NodePrimitive::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                       size_t) const {
    assert(isPrimitive(g.type()));

    switch (g.type()) {
        case AVRO_NULL:
            os << "null";
            break;
        case AVRO_BOOL:
            os << (g.value<bool>() ? "true" : "false");
            break;
        case AVRO_INT:
            os << g.value<int32_t>();
            break;
        case AVRO_LONG:
            os << g.value<int64_t>();
            break;
        case AVRO_FLOAT:
            os << g.value<float>();
            break;
        case AVRO_DOUBLE:
            os << g.value<double>();
            break;
        case AVRO_STRING:
            os << "\"" << escape(g.value<string>()) << "\"";
            break;
        case AVRO_BYTES: {
            // Convert to a string:
            const auto &vg = g.value<std::vector<uint8_t>>();
            string s;
            s.resize(vg.size() * kByteStringSize);
            for (unsigned int i = 0; i < vg.size(); i++) {
                string hex_string = intToHex(static_cast<int>(vg[i]));
                s.replace(i * kByteStringSize, kByteStringSize, hex_string);
            }
            os << "\"" << s << "\"";
        } break;
        default: break;
    }
}

void NodeEnum::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                  size_t) const {
    assert(g.type() == AVRO_ENUM);
    os << "\"" << g.value<GenericEnum>().symbol() << "\"";
}

void NodeFixed::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                   size_t) const {
    assert(g.type() == AVRO_FIXED);
    // ex: "\uOOff"
    // Convert to a string
    const std::vector<uint8_t> &vg = g.value<GenericFixed>().value();
    string s;
    s.resize(vg.size() * kByteStringSize);
    for (unsigned int i = 0; i < vg.size(); i++) {
        string hex_string = intToHex(static_cast<int>(vg[i]));
        s.replace(i * kByteStringSize, kByteStringSize, hex_string);
    }
    os << "\"" << s << "\"";
}

void NodeUnion::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                   size_t depth) const {
    leafAt(0)->printDefaultToJson(g, os, depth);
}

void NodeArray::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                   size_t depth) const {
    assert(g.type() == AVRO_ARRAY);
    // ex: "default": [1]
    if (g.value<GenericArray>().value().empty()) {
        os << "[]";
    } else {
        os << "[\n";
        depth++;

        // Serialize all values of the array with recursive calls:
        for (unsigned int i = 0; i < g.value<GenericArray>().value().size(); i++) {
            if (i > 0) {
                os << ",\n";
            }
            os << indent(depth);
            leafAt(0)->printDefaultToJson(g.value<GenericArray>().value()[i], os,
                                          depth);
        }
        os << "\n"
           << indent(--depth) << "]";
    }
}

void NodeSymbolic::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                      size_t depth) const {
    getNode()->printDefaultToJson(g, os, depth);
}

void NodeRecord::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                    size_t depth) const {
    assert(g.type() == AVRO_RECORD);
    if (g.value<GenericRecord>().fieldCount() == 0) {
        os << "{}";
    } else {
        os << "{\n";

        // Serialize all fields of the record with recursive calls:
        for (size_t i = 0; i < g.value<GenericRecord>().fieldCount(); i++) {
            if (i == 0) {
                ++depth;
            } else { // i > 0
                os << ",\n";
            }

            os << indent(depth) << "\"";
            assert(i < leaves());
            os << leafNameAttributes_.get(i);
            os << "\": ";

            // Recursive call on child node to be able to get the name attribute
            // (In case of a record we need the name of the leaves (contained in
            // 'this'))
            leafAt(i)->printDefaultToJson(g.value<GenericRecord>().fieldAt(i), os,
                                          depth);
        }
        os << "\n"
           << indent(--depth) << "}";
    }
}

NodeRecord::NodeRecord(const HasName &name, const MultiLeaves &fields,
                       const LeafNames &fieldsNames, std::vector<GenericDatum> dv)
    : NodeRecord(name, HasDoc(), fields, fieldsNames, {}, std::move(dv), MultiAttributes()) {}

NodeRecord::NodeRecord(const HasName &name, const HasDoc &doc, const MultiLeaves &fields,
                       const LeafNames &fieldsNames, std::vector<GenericDatum> dv)
    : NodeRecord(name, doc, fields, fieldsNames, {}, std::move(dv), MultiAttributes()) {}

NodeRecord::NodeRecord(const HasName &name, const MultiLeaves &fields,
                       const LeafNames &fieldsNames, std::vector<std::vector<std::string>> fieldsAliases,
                       std::vector<GenericDatum> dv, const MultiAttributes &customAttributes)
    : NodeRecord(name, HasDoc(), fields, fieldsNames, std::move(fieldsAliases), std::move(dv), customAttributes) {}

NodeRecord::NodeRecord(const HasName &name, const HasDoc &doc, const MultiLeaves &fields,
                       const LeafNames &fieldsNames, std::vector<std::vector<std::string>> fieldsAliases,
                       std::vector<GenericDatum> dv, const MultiAttributes &customAttributes)
    : NodeImplRecord(AVRO_RECORD, name, doc, fields, fieldsNames, customAttributes, NoSize()),
      fieldsAliases_(std::move(fieldsAliases)),
      fieldsDefaultValues_(std::move(dv)) {

    for (size_t i = 0; i < leafNameAttributes_.size(); ++i) {
        if (!nameIndex_.add(leafNameAttributes_.get(i), i)) {
            throw Exception("Cannot add duplicate field: {}", leafNameAttributes_.get(i));
        }

        if (!fieldsAliases_.empty()) {
            for (const auto &alias : fieldsAliases_[i]) {
                if (!nameIndex_.add(alias, i)) {
                    throw Exception("Cannot add duplicate field: {}", alias);
                }
            }
        }
    }
}

void NodeMap::printDefaultToJson(const GenericDatum &g, std::ostream &os,
                                 size_t depth) const {
    assert(g.type() == AVRO_MAP);
    if (g.value<GenericMap>().value().empty()) {
        os << "{}";
    } else {
        os << "{\n";

        for (size_t i = 0; i < g.value<GenericMap>().value().size(); i++) {
            if (i == 0) {
                ++depth;
            } else {
                os << ",\n";
            }
            os << indent(depth) << "\"" << g.value<GenericMap>().value()[i].first
               << "\": ";

            leafAt(i)->printDefaultToJson(g.value<GenericMap>().value()[i].second, os,
                                          depth);
        }
        os << "\n"
           << indent(--depth) << "}";
    }
}

void NodeEnum::printJson(std::ostream &os, size_t depth) const {
    os << "{\n";
    os << indent(++depth) << "\"type\": \"enum\",\n";
    if (!getDoc().empty()) {
        os << indent(depth) << R"("doc": ")"
           << escape(getDoc()) << "\",\n";
    }
    printName(os, nameAttribute_.get(), depth);
    os << indent(depth) << "\"symbols\": [\n";

    auto names = leafNameAttributes_.size();
    ++depth;
    for (size_t i = 0; i < names; ++i) {
        if (i > 0) {
            os << ",\n";
        }
        os << indent(depth) << '\"' << leafNameAttributes_.get(i) << '\"';
    }
    os << '\n';
    os << indent(--depth) << "]\n";
    os << indent(--depth) << '}';
}

void NodeArray::printJson(std::ostream &os, size_t depth) const {
    os << "{\n";
    os << indent(depth + 1) << "\"type\": \"array\",\n";
    if (!getDoc().empty()) {
        os << indent(depth + 1) << R"("doc": ")"
           << escape(getDoc()) << "\",\n";
    }
    os << indent(depth + 1) << "\"items\": ";
    leafAttributes_.get()->printJson(os, depth + 1);
    os << '\n';
    os << indent(depth) << '}';
}

void NodeMap::printJson(std::ostream &os, size_t depth) const {
    os << "{\n";
    os << indent(depth + 1) << "\"type\": \"map\",\n";
    if (!getDoc().empty()) {
        os << indent(depth + 1) << R"("doc": ")"
           << escape(getDoc()) << "\",\n";
    }
    os << indent(depth + 1) << "\"values\": ";
    leafAttributes_.get(1)->printJson(os, depth + 1);
    os << '\n';
    os << indent(depth) << '}';
}

NodeMap::NodeMap() : NodeImplMap(AVRO_MAP) {
    NodePtr key(new NodePrimitive(AVRO_STRING));
    doAddLeaf(key);
}

void NodeUnion::printJson(std::ostream &os, size_t depth) const {
    os << "[\n";
    auto fields = leafAttributes_.size();
    ++depth;
    for (size_t i = 0; i < fields; ++i) {
        if (i > 0) {
            os << ",\n";
        }
        os << indent(depth);
        leafAttributes_.get(i)->printJson(os, depth);
    }
    os << '\n';
    os << indent(--depth) << ']';
}

void NodeFixed::printJson(std::ostream &os, size_t depth) const {
    os << "{\n";
    os << indent(++depth) << "\"type\": \"fixed\",\n";
    if (!getDoc().empty()) {
        os << indent(depth) << R"("doc": ")"
           << escape(getDoc()) << "\",\n";
    }
    printName(os, nameAttribute_.get(), depth);
    os << indent(depth) << "\"size\": " << sizeAttribute_.get();

    if (logicalType().type() != LogicalType::NONE) {
        os << ",\n"
           << indent(depth);
        logicalType().printJson(os);
    }

    os << "\n"
       << indent(--depth) << '}';
}

} // namespace avro

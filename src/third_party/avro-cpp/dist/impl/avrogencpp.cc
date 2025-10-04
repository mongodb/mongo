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

#include <cctype>
#ifndef _WIN32
#include <ctime>
#endif
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>

#include <boost/random/mersenne_twister.hpp>
#include <utility>

#include "Compiler.hh"
#include "NodeImpl.hh"
#include "ValidSchema.hh"

using avro::NodePtr;
using avro::resolveSymbol;
using std::ifstream;
using std::map;
using std::ofstream;
using std::ostream;
using std::set;
using std::string;
using std::vector;

using boost::lexical_cast;

using avro::compileJsonSchema;
using avro::ValidSchema;

struct PendingSetterGetter {
    string structName;
    string type;
    string name;
    size_t idx;

    PendingSetterGetter(string sn, string t, string n, size_t i) : structName(std::move(sn)), type(std::move(t)), name(std::move(n)), idx(i) {}
};

struct PendingConstructor {
    string structName;
    string memberName;
    bool initMember;
    PendingConstructor(string sn, string n, bool im) : structName(std::move(sn)), memberName(std::move(n)), initMember(im) {}
};

class CodeGen {
    size_t unionNumber_;
    std::ostream &os_;
    bool inNamespace_;
    const std::string ns_;
    const std::string schemaFile_;
    const std::string headerFile_;
    const std::string includePrefix_;
    const bool noUnion_;
    const std::string guardString_;
    boost::mt19937 random_;

    vector<PendingSetterGetter> pendingGettersAndSetters;
    vector<PendingConstructor> pendingConstructors;

    map<NodePtr, string> done;
    set<NodePtr> doing;

    std::string guard();
    std::string fullname(const string &name) const;
    std::string generateEnumType(const NodePtr &n);
    std::string cppTypeOf(const NodePtr &n);
    std::string generateRecordType(const NodePtr &n);
    std::string unionName();
    std::string generateUnionType(const NodePtr &n);
    std::string generateType(const NodePtr &n);
    std::string generateDeclaration(const NodePtr &n);
    std::string doGenerateType(const NodePtr &n);
    void generateEnumTraits(const NodePtr &n);
    void generateTraits(const NodePtr &n);
    void generateRecordTraits(const NodePtr &n);
    void generateUnionTraits(const NodePtr &n);
    void emitCopyright();
    void emitGeneratedWarning();

public:
    CodeGen(std::ostream &os, std::string ns,
            std::string schemaFile, std::string headerFile,
            std::string guardString,
            std::string includePrefix, bool noUnion) : unionNumber_(0), os_(os), inNamespace_(false), ns_(std::move(ns)),
                                                       schemaFile_(std::move(schemaFile)), headerFile_(std::move(headerFile)),
                                                       includePrefix_(std::move(includePrefix)), noUnion_(noUnion),
                                                       guardString_(std::move(guardString)),
                                                       random_(static_cast<uint32_t>(::time(nullptr))) {
    }

    void generate(const ValidSchema &schema);
};

static string decorate(const std::string &name) {
    static const char *cppReservedWords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
        "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
        "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else",
        "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "reflexpr",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "synchronized", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};

    for (auto &cppReservedWord : cppReservedWords)
        if (strcmp(name.c_str(), cppReservedWord) == 0)
            return name + '_';
    return name;
}

static string decorate(const avro::Name &name) {
    return decorate(name.simpleName());
}

string CodeGen::fullname(const string &name) const {
    return ns_.empty() ? name : (ns_ + "::" + name);
}

string CodeGen::generateEnumType(const NodePtr &n) {
    string s = decorate(n->name());
    os_ << "enum class " << s << ": unsigned {\n";
    size_t c = n->names();
    for (size_t i = 0; i < c; ++i) {
        os_ << "    " << decorate(n->nameAt(i)) << ",\n";
    }
    os_ << "};\n\n";
    return s;
}

string CodeGen::cppTypeOf(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
            return "std::string";
        case avro::AVRO_BYTES:
            return "std::vector<uint8_t>";
        case avro::AVRO_INT:
            return "int32_t";
        case avro::AVRO_LONG:
            return "int64_t";
        case avro::AVRO_FLOAT:
            return "float";
        case avro::AVRO_DOUBLE:
            return "double";
        case avro::AVRO_BOOL:
            return "bool";
        case avro::AVRO_RECORD:
        case avro::AVRO_ENUM: {
            string nm = decorate(n->name());
            return inNamespace_ ? nm : fullname(nm);
        }
        case avro::AVRO_ARRAY:
            return "std::vector<" + cppTypeOf(n->leafAt(0)) + " >";
        case avro::AVRO_MAP:
            return "std::map<std::string, " + cppTypeOf(n->leafAt(1)) + " >";
        case avro::AVRO_FIXED:
            return "std::array<uint8_t, " + lexical_cast<string>(n->fixedSize()) + ">";
        case avro::AVRO_SYMBOLIC:
            return cppTypeOf(resolveSymbol(n));
        case avro::AVRO_UNION:
            return fullname(done[n]);
        case avro::AVRO_NULL:
            return "avro::null";
        default:
            return "$Undefined$";
    }
}

static string cppNameOf(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_NULL:
            return "null";
        case avro::AVRO_STRING:
            return "string";
        case avro::AVRO_BYTES:
            return "bytes";
        case avro::AVRO_INT:
            return "int";
        case avro::AVRO_LONG:
            return "long";
        case avro::AVRO_FLOAT:
            return "float";
        case avro::AVRO_DOUBLE:
            return "double";
        case avro::AVRO_BOOL:
            return "bool";
        case avro::AVRO_RECORD:
        case avro::AVRO_ENUM:
        case avro::AVRO_FIXED:
            return decorate(n->name());
        case avro::AVRO_ARRAY:
            return "array";
        case avro::AVRO_MAP:
            return "map";
        case avro::AVRO_SYMBOLIC:
            return cppNameOf(resolveSymbol(n));
        default:
            return "$Undefined$";
    }
}

string CodeGen::generateRecordType(const NodePtr &n) {
    size_t c = n->leaves();
    string decoratedName = decorate(n->name());
    vector<string> types;
    for (size_t i = 0; i < c; ++i) {
        types.push_back(generateType(n->leafAt(i)));
    }

    map<NodePtr, string>::const_iterator it = done.find(n);
    if (it != done.end()) {
        return it->second;
    }

    os_ << "struct " << decoratedName << " {\n";
    if (!noUnion_) {
        for (size_t i = 0; i < c; ++i) {
            if (n->leafAt(i)->type() == avro::AVRO_UNION) {
                os_ << "    typedef " << types[i]
                    << ' ' << n->nameAt(i) << "_t;\n";
                types[i] = n->nameAt(i) + "_t";
            }
            if (n->leafAt(i)->type() == avro::AVRO_ARRAY && n->leafAt(i)->leafAt(0)->type() == avro::AVRO_UNION) {
                os_ << "    typedef " << types[i] << "::value_type"
                    << ' ' << n->nameAt(i) << "_item_t;\n";
            }
        }
    }
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "    " << types[i];
        os_ << ' ' << decoratedNameAt << ";\n";
    }

    os_ << "    " << decoratedName << "()";
    if (c > 0) {
        os_ << " :";
    }
    os_ << "\n";
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "        " << decoratedNameAt << "(";
        os_ << types[i];
        os_ << "())";
        if (i != (c - 1)) {
            os_ << ',';
        }
        os_ << "\n";
    }
    os_ << "        { }\n";
    os_ << "};\n\n";
    return decoratedName;
}

void makeCanonical(string &s, bool foldCase) {
    for (char &c : s) {
        if (isalpha(c)) {
            if (foldCase) {
                c = static_cast<char>(toupper(c));
            }
        } else if (!isdigit(c)) {
            c = '_';
        }
    }
}

string CodeGen::unionName() {
    string s = schemaFile_;
    string::size_type n = s.find_last_of("/\\");
    if (n != string::npos) {
        s = s.substr(n);
    }
    makeCanonical(s, false);

    return s + "_Union__" + boost::lexical_cast<string>(unionNumber_++) + "__";
}

static void generateGetterAndSetter(ostream &os,
                                    const string &structName, const string &type, const string &name,
                                    size_t idx) {
    string sn = " " + structName + "::";

    os << "inline\n";

    os << type << sn << "get_" << name << "() const {\n"
       << "    if (idx_ != " << idx << ") {\n"
       << "        throw avro::Exception(\"Invalid type for "
       << "union " << structName << "\");\n"
       << "    }\n"
       << "    return std::any_cast<" << type << " >(value_);\n"
       << "}\n\n";

    os << "inline\n"
       << "void" << sn << "set_" << name
       << "(const " << type << "& v) {\n"
       << "    idx_ = " << idx << ";\n"
       << "    value_ = v;\n"
       << "}\n\n";
}

static void generateConstructor(ostream &os,
                                const string &structName, bool initMember,
                                const string &type) {
    os << "inline " << structName << "::" << structName << "() : idx_(0)";
    if (initMember) {
        os << ", value_(" << type << "())";
    }
    os << " { }\n";
}

/**
 * Generates a type for union and emits the code.
 * Since unions can encounter names that are not fully defined yet,
 * such names must be declared and the inline functions deferred until all
 * types are fully defined.
 */
string CodeGen::generateUnionType(const NodePtr &n) {
    size_t c = n->leaves();
    vector<string> types;
    vector<string> names;

    auto it = doing.find(n);
    if (it != doing.end()) {
        for (size_t i = 0; i < c; ++i) {
            const NodePtr &nn = n->leafAt(i);
            types.push_back(generateDeclaration(nn));
            names.push_back(cppNameOf(nn));
        }
    } else {
        doing.insert(n);
        for (size_t i = 0; i < c; ++i) {
            const NodePtr &nn = n->leafAt(i);
            types.push_back(generateType(nn));
            names.push_back(cppNameOf(nn));
        }
        doing.erase(n);
    }
    if (done.find(n) != done.end()) {
        return done[n];
    }

    auto result = unionName();

    os_ << "struct " << result << " {\n"
        << "private:\n"
        << "    size_t idx_;\n"
        << "    std::any value_;\n"
        << "public:\n"
        << "    size_t idx() const { return idx_; }\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "    bool is_null() const {\n"
                << "        return (idx_ == " << i << ");\n"
                << "    }\n"
                << "    void set_null() {\n"
                << "        idx_ = " << i << ";\n"
                << "        value_ = std::any();\n"
                << "    }\n";
        } else {
            const string &type = types[i];
            const string &name = names[i];
            os_ << "    " << type << " get_" << name << "() const;\n"
                                                        "    void set_"
                << name << "(const " << type << "& v);\n";
            pendingGettersAndSetters.emplace_back(result, type, name, i);
        }
    }

    os_ << "    " << result << "();\n";
    pendingConstructors.emplace_back(result, types[0],
                                     n->leafAt(0)->type() != avro::AVRO_NULL);
    os_ << "};\n\n";

    return result;
}

/**
 * Returns the type for the given schema node and emits code to os.
 */
string CodeGen::generateType(const NodePtr &n) {
    NodePtr nn = (n->type() == avro::AVRO_SYMBOLIC) ? resolveSymbol(n) : n;

    map<NodePtr, string>::const_iterator it = done.find(nn);
    if (it != done.end()) {
        return it->second;
    }
    string result = doGenerateType(nn);
    done[nn] = result;
    return result;
}

string CodeGen::doGenerateType(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
        case avro::AVRO_FIXED:
            return cppTypeOf(n);
        case avro::AVRO_ARRAY: {
            const NodePtr &ln = n->leafAt(0);
            string dn;
            if (doing.find(n) == doing.end()) {
                doing.insert(n);
                dn = generateType(ln);
                doing.erase(n);
            } else {
                dn = generateDeclaration(ln);
            }
            return "std::vector<" + dn + " >";
        }
        case avro::AVRO_MAP: {
            const NodePtr &ln = n->leafAt(1);
            string dn;
            if (doing.find(n) == doing.end()) {
                doing.insert(n);
                dn = generateType(ln);
                doing.erase(n);
            } else {
                dn = generateDeclaration(ln);
            }
            return "std::map<std::string, " + dn + " >";
        }
        case avro::AVRO_RECORD:
            return generateRecordType(n);
        case avro::AVRO_ENUM:
            return generateEnumType(n);
        case avro::AVRO_UNION:
            return generateUnionType(n);
        default:
            break;
    }
    return "$Undefined$";
}

string CodeGen::generateDeclaration(const NodePtr &n) {
    NodePtr nn = (n->type() == avro::AVRO_SYMBOLIC) ? resolveSymbol(n) : n;
    switch (nn->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
        case avro::AVRO_FIXED:
            return cppTypeOf(nn);
        case avro::AVRO_ARRAY:
            return "std::vector<" + generateDeclaration(nn->leafAt(0)) + " >";
        case avro::AVRO_MAP:
            return "std::map<std::string, " + generateDeclaration(nn->leafAt(1)) + " >";
        case avro::AVRO_RECORD:
            os_ << "struct " << cppTypeOf(nn) << ";\n";
            return cppTypeOf(nn);
        case avro::AVRO_ENUM:
            return generateEnumType(nn);
        case avro::AVRO_UNION:
            // FIXME: When can this happen?
            return generateUnionType(nn);
        default:
            break;
    }
    return "$Undefined$";
}

void CodeGen::generateEnumTraits(const NodePtr &n) {
    string dname = decorate(n->name());
    string fn = fullname(dname);

    // the nameAt(i) does not take c++ reserved words into account
    // so we need to call decorate on it
    string last = decorate(n->nameAt(n->names() - 1));

    os_ << "template<> struct codec_traits<" << fn << "> {\n"
        << "    static void encode(Encoder& e, " << fn << " v) {\n"
        << "        if (v > " << fn << "::" << last << ")\n"
        << "        {\n"
        << "            std::ostringstream error;\n"
        << R"(            error << "enum value " << static_cast<unsigned>(v) << " is out of bound for )" << fn
        << " and cannot be encoded\";\n"
        << "            throw avro::Exception(error.str());\n"
        << "        }\n"
        << "        e.encodeEnum(static_cast<size_t>(v));\n"
        << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n"
        << "        size_t index = d.decodeEnum();\n"
        << "        if (index > static_cast<size_t>(" << fn << "::" << last << "))\n"
        << "        {\n"
        << "            std::ostringstream error;\n"
        << R"(            error << "enum value " << index << " is out of bound for )" << fn
        << " and cannot be decoded\";\n"
        << "            throw avro::Exception(error.str());\n"
        << "        }\n"
        << "        v = static_cast<" << fn << ">(index);\n"
        << "    }\n"
        << "};\n\n";
}

void CodeGen::generateRecordTraits(const NodePtr &n) {
    size_t c = n->leaves();
    for (size_t i = 0; i < c; ++i) {
        generateTraits(n->leafAt(i));
    }

    string fn = fullname(decorate(n->name()));
    os_ << "template<> struct codec_traits<" << fn << "> {\n";

    if (c == 0) {
        os_ << "    static void encode(Encoder&, const " << fn << "&) {}\n";
        // ResolvingDecoder::fieldOrder mutates the state of the decoder, so if that decoder is
        // passed in, we need to call the method even though it will return an empty vector.
        os_ << "    static void decode(Decoder& d, " << fn << "&) {\n";
        os_ << "        if (avro::ResolvingDecoder *rd = dynamic_cast<avro::ResolvingDecoder *>(&d)) {\n";
        os_ << "            rd->fieldOrder();\n";
        os_ << "        }\n";
        os_ << "    }\n";
        os_ << "};\n";
        return;
    }

    os_ << "    static void encode(Encoder& e, const " << fn << "& v) {\n";

    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "        avro::encode(e, v." << decoratedNameAt << ");\n";
    }

    os_ << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n";
    os_ << "        if (avro::ResolvingDecoder *rd =\n";
    os_ << "            dynamic_cast<avro::ResolvingDecoder *>(&d)) {\n";
    os_ << "            const std::vector<size_t> fo = rd->fieldOrder();\n";
    os_ << "            for (std::vector<size_t>::const_iterator it = fo.begin();\n";
    os_ << "                it != fo.end(); ++it) {\n";
    os_ << "                switch (*it) {\n";
    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "                case " << i << ":\n";
        os_ << "                    avro::decode(d, v." << decoratedNameAt << ");\n";
        os_ << "                    break;\n";
    }
    os_ << "                default:\n";
    os_ << "                    break;\n";
    os_ << "                }\n";
    os_ << "            }\n";
    os_ << "        } else {\n";

    for (size_t i = 0; i < c; ++i) {
        // the nameAt(i) does not take c++ reserved words into account
        // so we need to call decorate on it
        std::string decoratedNameAt = decorate(n->nameAt(i));
        os_ << "            avro::decode(d, v." << decoratedNameAt << ");\n";
    }
    os_ << "        }\n";

    os_ << "    }\n"
        << "};\n\n";
}

void CodeGen::generateUnionTraits(const NodePtr &n) {
    size_t c = n->leaves();

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        generateTraits(nn);
    }

    string name = done[n];
    string fn = fullname(name);

    os_ << "template<> struct codec_traits<" << fn << "> {\n"
        << "    static void encode(Encoder& e, " << fn << " v) {\n"
        << "        e.encodeUnionIndex(v.idx());\n"
        << "        switch (v.idx()) {\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        os_ << "        case " << i << ":\n";
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "            e.encodeNull();\n";
        } else {
            os_ << "            avro::encode(e, v.get_" << cppNameOf(nn)
                << "());\n";
        }
        os_ << "            break;\n";
    }

    os_ << "        }\n"
        << "    }\n"
        << "    static void decode(Decoder& d, " << fn << "& v) {\n"
        << "        size_t n = d.decodeUnionIndex();\n"
        << "        if (n >= " << c << ") { throw avro::Exception(\""
                                       "Union index too big\"); }\n"
        << "        switch (n) {\n";

    for (size_t i = 0; i < c; ++i) {
        const NodePtr &nn = n->leafAt(i);
        os_ << "        case " << i << ":\n";
        if (nn->type() == avro::AVRO_NULL) {
            os_ << "            d.decodeNull();\n"
                << "            v.set_null();\n";
        } else {
            os_ << "            {\n"
                << "                " << cppTypeOf(nn) << " vv;\n"
                << "                avro::decode(d, vv);\n"
                << "                v.set_" << cppNameOf(nn) << "(vv);\n"
                << "            }\n";
        }
        os_ << "            break;\n";
    }
    os_ << "        }\n"
        << "    }\n"
        << "};\n\n";
}

void CodeGen::generateTraits(const NodePtr &n) {
    switch (n->type()) {
        case avro::AVRO_STRING:
        case avro::AVRO_BYTES:
        case avro::AVRO_INT:
        case avro::AVRO_LONG:
        case avro::AVRO_FLOAT:
        case avro::AVRO_DOUBLE:
        case avro::AVRO_BOOL:
        case avro::AVRO_NULL:
            break;
        case avro::AVRO_RECORD:
            generateRecordTraits(n);
            break;
        case avro::AVRO_ENUM:
            generateEnumTraits(n);
            break;
        case avro::AVRO_ARRAY:
        case avro::AVRO_MAP:
            generateTraits(n->leafAt(n->type() == avro::AVRO_ARRAY ? 0 : 1));
            break;
        case avro::AVRO_UNION:
            generateUnionTraits(n);
            break;
        case avro::AVRO_FIXED:
        default:
            break;
    }
}

void CodeGen::emitCopyright() {
    os_ << "/**\n"
           " * Licensed to the Apache Software Foundation (ASF) under one\n"
           " * or more contributor license agreements.  See the NOTICE file\n"
           " * distributed with this work for additional information\n"
           " * regarding copyright ownership.  The ASF licenses this file\n"
           " * to you under the Apache License, Version 2.0 (the\n"
           " * \"License\"); you may not use this file except in compliance\n"
           " * with the License.  You may obtain a copy of the License at\n"
           " *\n"
           " *     https://www.apache.org/licenses/LICENSE-2.0\n"
           " *\n"
           " * Unless required by applicable law or agreed to in writing, "
           "software\n"
           " * distributed under the License is distributed on an "
           "\"AS IS\" BASIS,\n"
           " * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express "
           "or implied.\n"
           " * See the License for the specific language governing "
           "permissions and\n"
           " * limitations under the License.\n"
           " */\n\n";
}

void CodeGen::emitGeneratedWarning() {
    os_ << "/* This code was generated by avrogencpp " << AVRO_VERSION << ". Do not edit.*/\n\n";
}

string CodeGen::guard() {
    string h = headerFile_;
    makeCanonical(h, true);
    return h + "_" + lexical_cast<string>(random_()) + "_H";
}

void CodeGen::generate(const ValidSchema &schema) {
    emitCopyright();
    emitGeneratedWarning();

    string h = guardString_.empty() ? guard() : guardString_;

    os_ << "#ifndef " << h << "\n";
    os_ << "#define " << h << "\n\n\n";

    os_ << "#include <sstream>\n"
        << "#include <any>\n"
        << "#include \"" << includePrefix_ << "Specific.hh\"\n"
        << "#include \"" << includePrefix_ << "Encoder.hh\"\n"
        << "#include \"" << includePrefix_ << "Decoder.hh\"\n"
        << "\n";

    if (!ns_.empty()) {
        os_ << "namespace " << ns_ << " {\n";
        inNamespace_ = true;
    }

    const NodePtr &root = schema.root();
    generateType(root);

    for (vector<PendingSetterGetter>::const_iterator it =
             pendingGettersAndSetters.begin();
         it != pendingGettersAndSetters.end(); ++it) {
        generateGetterAndSetter(os_, it->structName, it->type, it->name,
                                it->idx);
    }

    for (vector<PendingConstructor>::const_iterator it =
             pendingConstructors.begin();
         it != pendingConstructors.end(); ++it) {
        generateConstructor(os_, it->structName,
                            it->initMember, it->memberName);
    }

    if (!ns_.empty()) {
        inNamespace_ = false;
        os_ << "}\n";
    }

    os_ << "namespace avro {\n";

    unionNumber_ = 0;

    generateTraits(root);

    os_ << "}\n";

    os_ << "#endif\n";
    os_.flush();
}

namespace po = boost::program_options;

static string readGuard(const string &filename) {
    std::ifstream ifs(filename.c_str());
    string buf;
    string candidate;
    while (std::getline(ifs, buf)) {
        boost::algorithm::trim(buf);
        if (candidate.empty()) {
            if (boost::algorithm::starts_with(buf, "#ifndef ")) {
                candidate = buf.substr(8);
            }
        } else if (boost::algorithm::starts_with(buf, "#define ")) {
            if (candidate == buf.substr(8)) {
                break;
            }
        } else {
            candidate.erase();
        }
    }
    return candidate;
}

int main(int argc, char **argv) {
    const string NS("namespace");
    const string OUT_FILE("output");
    const string IN_FILE("input");
    const string INCLUDE_PREFIX("include-prefix");
    const string NO_UNION_TYPEDEF("no-union-typedef");

    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help,h", "produce help message")
        ("version,V", "produce version information")
        ("include-prefix,p", po::value<string>()->default_value("avro"), "prefix for include headers, - for none, default: avro")
        ("no-union-typedef,U", "do not generate typedefs for unions in records")
        ("namespace,n", po::value<string>(), "set namespace for generated code")
        ("input,i", po::value<string>(), "input file")
        ("output,o", po::value<string>(), "output file to generate");
    // clang-format on

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    if (vm.count("version")) {
        std::cout << AVRO_VERSION << std::endl;
        return 0;
    }

    if (vm.count(IN_FILE) == 0 || vm.count(OUT_FILE) == 0) {
        std::cout << desc << std::endl;
        return 1;
    }

    string ns = vm.count(NS) > 0 ? vm[NS].as<string>() : string();
    string outf = vm.count(OUT_FILE) > 0 ? vm[OUT_FILE].as<string>() : string();
    string inf = vm.count(IN_FILE) > 0 ? vm[IN_FILE].as<string>() : string();
    string incPrefix = vm[INCLUDE_PREFIX].as<string>();
    bool noUnion = vm.count(NO_UNION_TYPEDEF) != 0;

    if (incPrefix == "-") {
        incPrefix.clear();
    } else if (*incPrefix.rbegin() != '/') {
        incPrefix += "/";
    }

    try {
        ValidSchema schema;

        if (!inf.empty()) {
            ifstream in(inf.c_str());
            compileJsonSchema(in, schema);
        } else {
            compileJsonSchema(std::cin, schema);
        }

        if (!outf.empty()) {
            string g = readGuard(outf);
            ofstream out(outf.c_str());
            CodeGen(out, ns, inf, outf, g, incPrefix, noUnion).generate(schema);
        } else {
            CodeGen(std::cout, ns, inf, outf, "", incPrefix, noUnion).generate(schema);
        }
        return 0;
    } catch (std::exception &e) {
        std::cerr << "Failed to parse or compile schema: "
                  << e.what() << std::endl;
        return 1;
    }
}

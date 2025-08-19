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

#include <algorithm>
#include <boost/math/special_functions/fpclassify.hpp>
#include <map>
#include <memory>
#include <string>

#include "Decoder.hh"
#include "Encoder.hh"
#include "Symbol.hh"
#include "ValidSchema.hh"
#include "ValidatingCodec.hh"

#include "../json/JsonIO.hh"

namespace avro {

namespace parsing {

using std::make_shared;

using std::istringstream;
using std::map;
using std::ostringstream;
using std::reverse;
using std::string;
using std::vector;

using avro::json::JsonGenerator;
using avro::json::JsonNullFormatter;
using avro::json::JsonParser;

class JsonGrammarGenerator : public ValidatingGrammarGenerator {
    ProductionPtr doGenerate(const NodePtr &n,
                             std::map<NodePtr, ProductionPtr> &m) final;
};

static std::string nameOf(const NodePtr &n) {
    if (n->hasName()) {
        return std::string(n->name());
    }
    std::ostringstream oss;
    oss << n->type();
    return oss.str();
}

ProductionPtr JsonGrammarGenerator::doGenerate(const NodePtr &n,
                                               std::map<NodePtr, ProductionPtr> &m) {
    switch (n->type()) {
        case AVRO_NULL:
        case AVRO_BOOL:
        case AVRO_INT:
        case AVRO_LONG:
        case AVRO_FLOAT:
        case AVRO_DOUBLE:
        case AVRO_STRING:
        case AVRO_BYTES:
        case AVRO_FIXED:
        case AVRO_ARRAY:
        case AVRO_MAP:
        case AVRO_SYMBOLIC:
            return ValidatingGrammarGenerator::doGenerate(n, m);
        case AVRO_RECORD: {
            ProductionPtr result = make_shared<Production>();

            m.erase(n);

            size_t c = n->leaves();
            result->reserve(2 + 2 * c);
            result->push_back(Symbol::recordStartSymbol());
            for (size_t i = 0; i < c; ++i) {
                const NodePtr &leaf = n->leafAt(i);
                ProductionPtr v = doGenerate(leaf, m);
                result->push_back(Symbol::fieldSymbol(n->nameAt(i)));
                copy(v->rbegin(), v->rend(), back_inserter(*result));
            }
            result->push_back(Symbol::recordEndSymbol());
            reverse(result->begin(), result->end());

            m[n] = result;
            return make_shared<Production>(1, Symbol::indirect(result));
        }
        case AVRO_ENUM: {
            vector<string> nn;
            size_t c = n->names();
            nn.reserve(c);
            for (size_t i = 0; i < c; ++i) {
                nn.push_back(n->nameAt(i));
            }
            ProductionPtr result = make_shared<Production>();
            result->push_back(Symbol::nameListSymbol(nn));
            result->push_back(Symbol::enumSymbol());
            m[n] = result;
            return result;
        }
        case AVRO_UNION: {
            size_t c = n->leaves();

            vector<ProductionPtr> vv;
            vv.reserve(c);

            vector<string> names;
            names.reserve(c);

            for (size_t i = 0; i < c; ++i) {
                const NodePtr &nn = n->leafAt(i);
                ProductionPtr v = doGenerate(nn, m);
                if (nn->type() != AVRO_NULL) {
                    ProductionPtr v2 = make_shared<Production>();
                    v2->push_back(Symbol::recordEndSymbol());
                    copy(v->begin(), v->end(), back_inserter(*v2));
                    v.swap(v2);
                }
                vv.push_back(v);
                names.push_back(nameOf(nn));
            }
            ProductionPtr result = make_shared<Production>();
            result->push_back(Symbol::alternative(vv));
            result->push_back(Symbol::nameListSymbol(names));
            result->push_back(Symbol::unionSymbol());
            return result;
        }
        default:
            throw Exception("Unknown node type");
    }
}

static void expectToken(JsonParser &in, JsonParser::Token tk) {
    in.expectToken(tk);
}

class JsonDecoderHandler {
    JsonParser &in_;

public:
    explicit JsonDecoderHandler(JsonParser &p) : in_(p) {}
    size_t handle(const Symbol &s) {
        switch (s.kind()) {
            case Symbol::Kind::RecordStart:
                expectToken(in_, JsonParser::Token::ObjectStart);
                break;
            case Symbol::Kind::RecordEnd:
                expectToken(in_, JsonParser::Token::ObjectEnd);
                break;
            case Symbol::Kind::Field:
                expectToken(in_, JsonParser::Token::String);
                if (s.extra<string>() != in_.stringValue()) {
                    throw Exception(R"(Incorrect field: expected "{}" but got "{}".)", s.extra<string>(), in_.stringValue());
                }
                break;
            default:
                break;
        }
        return 0;
    }
};

template<typename P>
class JsonDecoder : public Decoder {
    JsonParser in_;
    JsonDecoderHandler handler_;
    P parser_;

    void init(InputStream &is) final;
    void decodeNull() final;
    bool decodeBool() final;
    int32_t decodeInt() final;
    int64_t decodeLong() final;
    float decodeFloat() final;
    double decodeDouble() final;
    void decodeString(string &value) final;
    void skipString() final;
    void decodeBytes(vector<uint8_t> &value) final;
    void skipBytes() final;
    void decodeFixed(size_t n, vector<uint8_t> &value) final;
    void skipFixed(size_t n) final;
    size_t decodeEnum() final;
    size_t arrayStart() final;
    size_t arrayNext() final;
    size_t skipArray() final;
    size_t mapStart() final;
    size_t mapNext() final;
    size_t skipMap() final;
    size_t decodeUnionIndex() final;

    void expect(JsonParser::Token tk);
    void skipComposite();
    void drain() final;

public:
    explicit JsonDecoder(const ValidSchema &s) : handler_(in_),
                                                 parser_(JsonGrammarGenerator().generate(s), NULL, handler_) {}
};

template<typename P>
void JsonDecoder<P>::init(InputStream &is) {
    in_.init(is);
    parser_.reset();
}

template<typename P>
void JsonDecoder<P>::expect(JsonParser::Token tk) {
    expectToken(in_, tk);
}

template<typename P>
void JsonDecoder<P>::decodeNull() {
    parser_.advance(Symbol::Kind::Null);
    expect(JsonParser::Token::Null);
}

template<typename P>
bool JsonDecoder<P>::decodeBool() {
    parser_.advance(Symbol::Kind::Bool);
    expect(JsonParser::Token::Bool);
    bool result = in_.boolValue();
    return result;
}

template<typename P>
int32_t JsonDecoder<P>::decodeInt() {
    parser_.advance(Symbol::Kind::Int);
    expect(JsonParser::Token::Long);
    int64_t result = in_.longValue();
    if (result < INT32_MIN || result > INT32_MAX) {
        throw Exception("Value out of range for Avro int: {}", result);
    }
    return static_cast<int32_t>(result);
}

template<typename P>
int64_t JsonDecoder<P>::decodeLong() {
    parser_.advance(Symbol::Kind::Long);
    expect(JsonParser::Token::Long);
    int64_t result = in_.longValue();
    return result;
}

template<typename P>
float JsonDecoder<P>::decodeFloat() {
    parser_.advance(Symbol::Kind::Float);
    expect(JsonParser::Token::Double);
    double result = in_.doubleValue();
    return static_cast<float>(result);
}

template<typename P>
double JsonDecoder<P>::decodeDouble() {
    parser_.advance(Symbol::Kind::Double);
    expect(JsonParser::Token::Double);
    double result = in_.doubleValue();
    return result;
}

template<typename P>
void JsonDecoder<P>::decodeString(string &value) {
    parser_.advance(Symbol::Kind::String);
    expect(JsonParser::Token::String);
    value = in_.stringValue();
}

template<typename P>
void JsonDecoder<P>::skipString() {
    parser_.advance(Symbol::Kind::String);
    expect(JsonParser::Token::String);
}

static vector<uint8_t> toBytes(const string &s) {
    return vector<uint8_t>(s.begin(), s.end());
}

template<typename P>
void JsonDecoder<P>::decodeBytes(vector<uint8_t> &value) {
    parser_.advance(Symbol::Kind::Bytes);
    expect(JsonParser::Token::String);
    value = toBytes(in_.bytesValue());
}

template<typename P>
void JsonDecoder<P>::skipBytes() {
    parser_.advance(Symbol::Kind::Bytes);
    expect(JsonParser::Token::String);
}

template<typename P>
void JsonDecoder<P>::decodeFixed(size_t n, vector<uint8_t> &value) {
    parser_.advance(Symbol::Kind::Fixed);
    parser_.assertSize(n);
    expect(JsonParser::Token::String);
    value = toBytes(in_.bytesValue());
    if (value.size() != n) {
        throw Exception("Incorrect value for fixed");
    }
}

template<typename P>
void JsonDecoder<P>::skipFixed(size_t n) {
    parser_.advance(Symbol::Kind::Fixed);
    parser_.assertSize(n);
    expect(JsonParser::Token::String);
    vector<uint8_t> result = toBytes(in_.bytesValue());
    if (result.size() != n) {
        throw Exception("Incorrect value for fixed");
    }
}

template<typename P>
size_t JsonDecoder<P>::decodeEnum() {
    parser_.advance(Symbol::Kind::Enum);
    expect(JsonParser::Token::String);
    size_t result = parser_.indexForName(in_.stringValue());
    return result;
}

template<typename P>
size_t JsonDecoder<P>::arrayStart() {
    parser_.advance(Symbol::Kind::ArrayStart);
    parser_.pushRepeatCount(0);
    expect(JsonParser::Token::ArrayStart);
    return arrayNext();
}

template<typename P>
size_t JsonDecoder<P>::arrayNext() {
    parser_.processImplicitActions();
    if (in_.peek() == JsonParser::Token::ArrayEnd) {
        in_.advance();
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::ArrayEnd);
        return 0;
    }
    parser_.nextRepeatCount(1);
    return 1;
}

template<typename P>
void JsonDecoder<P>::skipComposite() {
    size_t level = 0;
    for (;;) {
        switch (in_.advance()) {
            case JsonParser::Token::ArrayStart:
            case JsonParser::Token::ObjectStart:
                ++level;
                continue;
            case JsonParser::Token::ArrayEnd:
            case JsonParser::Token::ObjectEnd:
                if (level == 0) {
                    return;
                }
                --level;
                continue;
            default:
                continue;
        }
    }
}

template<typename P>
void JsonDecoder<P>::drain() {
    parser_.processImplicitActions();
    in_.drain();
}

template<typename P>
size_t JsonDecoder<P>::skipArray() {
    parser_.advance(Symbol::Kind::ArrayStart);
    parser_.pop();
    parser_.advance(Symbol::Kind::ArrayEnd);
    expect(JsonParser::Token::ArrayStart);
    skipComposite();
    return 0;
}

template<typename P>
size_t JsonDecoder<P>::mapStart() {
    parser_.advance(Symbol::Kind::MapStart);
    parser_.pushRepeatCount(0);
    expect(JsonParser::Token::ObjectStart);
    return mapNext();
}

template<typename P>
size_t JsonDecoder<P>::mapNext() {
    parser_.processImplicitActions();
    if (in_.peek() == JsonParser::Token::ObjectEnd) {
        in_.advance();
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::MapEnd);
        return 0;
    }
    parser_.nextRepeatCount(1);
    return 1;
}

template<typename P>
size_t JsonDecoder<P>::skipMap() {
    parser_.advance(Symbol::Kind::MapStart);
    parser_.pop();
    parser_.advance(Symbol::Kind::MapEnd);
    expect(JsonParser::Token::ObjectStart);
    skipComposite();
    return 0;
}

template<typename P>
size_t JsonDecoder<P>::decodeUnionIndex() {
    parser_.advance(Symbol::Kind::Union);

    size_t result;
    if (in_.peek() == JsonParser::Token::Null) {
        result = parser_.indexForName("null");
    } else {
        expect(JsonParser::Token::ObjectStart);
        expect(JsonParser::Token::String);
        result = parser_.indexForName(in_.stringValue());
    }
    parser_.selectBranch(result);
    return result;
}

template<typename F = JsonNullFormatter>
class JsonHandler {
    JsonGenerator<F> &generator_;

public:
    explicit JsonHandler(JsonGenerator<F> &g) : generator_(g) {}
    size_t handle(const Symbol &s) {
        switch (s.kind()) {
            case Symbol::Kind::RecordStart:
                generator_.objectStart();
                break;
            case Symbol::Kind::RecordEnd:
                generator_.objectEnd();
                break;
            case Symbol::Kind::Field:
                generator_.encodeString(s.extra<string>());
                break;
            default:
                break;
        }
        return 0;
    }
};

template<typename P, typename F = JsonNullFormatter>
class JsonEncoder : public Encoder {
    JsonGenerator<F> out_;
    JsonHandler<F> handler_;
    P parser_;

    void init(OutputStream &os) final;
    void flush() final;
    int64_t byteCount() const final;
    void encodeNull() final;
    void encodeBool(bool b) final;
    void encodeInt(int32_t i) final;
    void encodeLong(int64_t l) final;
    void encodeFloat(float f) final;
    void encodeDouble(double d) final;
    void encodeString(const std::string &s) final;
    void encodeBytes(const uint8_t *bytes, size_t len) final;
    void encodeFixed(const uint8_t *bytes, size_t len) final;
    void encodeEnum(size_t e) final;
    void arrayStart() final;
    void arrayEnd() final;
    void mapStart() final;
    void mapEnd() final;
    void setItemCount(size_t count) final;
    void startItem() final;
    void encodeUnionIndex(size_t e) final;

public:
    explicit JsonEncoder(const ValidSchema &schema) : handler_(out_),
                                                      parser_(JsonGrammarGenerator().generate(schema), NULL, handler_) {}
};

template<typename P, typename F>
void JsonEncoder<P, F>::init(OutputStream &os) {
    out_.init(os);
    parser_.reset();
}

template<typename P, typename F>
void JsonEncoder<P, F>::flush() {
    parser_.processImplicitActions();
    out_.flush();
}

template<typename P, typename F>
int64_t JsonEncoder<P, F>::byteCount() const {
    return out_.byteCount();
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeNull() {
    parser_.advance(Symbol::Kind::Null);
    out_.encodeNull();
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeBool(bool b) {
    parser_.advance(Symbol::Kind::Bool);
    out_.encodeBool(b);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeInt(int32_t i) {
    parser_.advance(Symbol::Kind::Int);
    out_.encodeNumber(i);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeLong(int64_t l) {
    parser_.advance(Symbol::Kind::Long);
    out_.encodeNumber(l);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeFloat(float f) {
    parser_.advance(Symbol::Kind::Float);
    if (f == std::numeric_limits<float>::infinity()) {
        out_.encodeString("Infinity");
    } else if (-f == std::numeric_limits<float>::infinity()) {
        out_.encodeString("-Infinity");
    } else if (boost::math::isnan(f)) {
        out_.encodeString("NaN");
    } else {
        out_.encodeNumber(f);
    }
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeDouble(double d) {
    parser_.advance(Symbol::Kind::Double);
    if (d == std::numeric_limits<double>::infinity()) {
        out_.encodeString("Infinity");
    } else if (-d == std::numeric_limits<double>::infinity()) {
        out_.encodeString("-Infinity");
    } else if (boost::math::isnan(d)) {
        out_.encodeString("NaN");
    } else {
        out_.encodeNumber(d);
    }
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeString(const std::string &s) {
    parser_.advance(Symbol::Kind::String);
    out_.encodeString(s);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeBytes(const uint8_t *bytes, size_t len) {
    parser_.advance(Symbol::Kind::Bytes);
    out_.encodeBinary(bytes, len);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeFixed(const uint8_t *bytes, size_t len) {
    parser_.advance(Symbol::Kind::Fixed);
    parser_.assertSize(len);
    out_.encodeBinary(bytes, len);
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeEnum(size_t e) {
    parser_.advance(Symbol::Kind::Enum);
    const string &s = parser_.nameForIndex(e);
    out_.encodeString(s);
}

template<typename P, typename F>
void JsonEncoder<P, F>::arrayStart() {
    parser_.advance(Symbol::Kind::ArrayStart);
    parser_.pushRepeatCount(0);
    out_.arrayStart();
}

template<typename P, typename F>
void JsonEncoder<P, F>::arrayEnd() {
    parser_.popRepeater();
    parser_.advance(Symbol::Kind::ArrayEnd);
    out_.arrayEnd();
}

template<typename P, typename F>
void JsonEncoder<P, F>::mapStart() {
    parser_.advance(Symbol::Kind::MapStart);
    parser_.pushRepeatCount(0);
    out_.objectStart();
}

template<typename P, typename F>
void JsonEncoder<P, F>::mapEnd() {
    parser_.popRepeater();
    parser_.advance(Symbol::Kind::MapEnd);
    out_.objectEnd();
}

template<typename P, typename F>
void JsonEncoder<P, F>::setItemCount(size_t count) {
    parser_.nextRepeatCount(count);
}

template<typename P, typename F>
void JsonEncoder<P, F>::startItem() {
    parser_.processImplicitActions();
    if (parser_.top() != Symbol::Kind::Repeater) {
        throw Exception("startItem at not an item boundary");
    }
}

template<typename P, typename F>
void JsonEncoder<P, F>::encodeUnionIndex(size_t e) {
    parser_.advance(Symbol::Kind::Union);

    const std::string name = parser_.nameForIndex(e);

    if (name != "null") {
        out_.objectStart();
        out_.encodeString(name);
    }
    parser_.selectBranch(e);
}

} // namespace parsing

DecoderPtr jsonDecoder(const ValidSchema &s) {
    return std::make_shared<parsing::JsonDecoder<
        parsing::SimpleParser<parsing::JsonDecoderHandler>>>(s);
}

EncoderPtr jsonEncoder(const ValidSchema &schema) {
    return std::make_shared<parsing::JsonEncoder<
        parsing::SimpleParser<parsing::JsonHandler<avro::json::JsonNullFormatter>>, avro::json::JsonNullFormatter>>(schema);
}

EncoderPtr jsonPrettyEncoder(const ValidSchema &schema) {
    return std::make_shared<parsing::JsonEncoder<
        parsing::SimpleParser<parsing::JsonHandler<avro::json::JsonPrettyFormatter>>, avro::json::JsonPrettyFormatter>>(schema);
}

} // namespace avro

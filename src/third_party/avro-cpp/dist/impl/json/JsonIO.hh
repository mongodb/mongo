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

#ifndef avro_json_JsonIO_hh__
#define avro_json_JsonIO_hh__

#include <boost/lexical_cast.hpp>
#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/utility.hpp>
#include <locale>
#include <sstream>
#include <stack>
#include <string>

#include "Config.hh"
#include "Stream.hh"

namespace avro {
namespace json {

inline char toHex(unsigned int n) {
    return static_cast<char>((n < 10) ? (n + '0') : (n + 'a' - 10));
}

class AVRO_DECL JsonParser : boost::noncopyable {
public:
    enum class Token {
        Null,
        Bool,
        Long,
        Double,
        String,
        ArrayStart,
        ArrayEnd,
        ObjectStart,
        ObjectEnd
    };

    size_t line() const { return line_; }

private:
    enum State {
        stValue,   // Expect a data type
        stArray0,  // Expect a data type or ']'
        stArrayN,  // Expect a ',' or ']'
        stObject0, // Expect a string or a '}'
        stObjectN, // Expect a ',' or '}'
        stKey      // Expect a ':'
    };
    std::stack<State> stateStack;
    State curState;
    bool hasNext;
    char nextChar;
    bool peeked;

    StreamReader in_;
    Token curToken;
    bool bv;
    int64_t lv;
    double dv;
    std::string sv;
    size_t line_;

    Token doAdvance();
    Token tryLiteral(const char exp[], size_t n, Token tk);
    Token tryNumber(char ch);
    Token tryString();
    static Exception unexpected(unsigned char ch);
    char next();

    static std::string decodeString(const std::string &s, bool binary);

public:
    JsonParser() : curState(stValue), hasNext(false), nextChar(0), peeked(false),
                   curToken(Token::Null), bv(false), lv(0), dv(0), line_(1) {}

    void init(InputStream &is) {
        // Clear by swapping with an empty stack
        std::stack<State>().swap(stateStack);
        curState = stValue;
        hasNext = false;
        peeked = false;
        line_ = 1;
        in_.reset(is);
    }

    Token advance() {
        if (!peeked) {
            curToken = doAdvance();
        } else {
            peeked = false;
        }
        return curToken;
    }

    Token peek() {
        if (!peeked) {
            curToken = doAdvance();
            peeked = true;
        }
        return curToken;
    }

    void expectToken(Token tk);

    bool boolValue() const {
        return bv;
    }

    Token cur() const {
        return curToken;
    }

    double doubleValue() const {
        return dv;
    }

    int64_t longValue() const {
        return lv;
    }

    const std::string &rawString() const {
        return sv;
    }

    std::string stringValue() const {
        return decodeString(sv, false);
    }

    std::string bytesValue() const {
        return decodeString(sv, true);
    }

    void drain() {
        if (!stateStack.empty() || peeked) {
            throw Exception("Invalid state for draining");
        }
        in_.drain(hasNext);
        hasNext = false;
    }

    /**
     * Return UTF-8 encoded string value.
     */
    static std::string toStringValue(const std::string &sv) {
        return decodeString(sv, false);
    }

    /**
     * Return byte-encoded string value. It is an error if the input
     * JSON string contained unicode characters more than "\u00ff'.
     */
    static std::string toBytesValue(const std::string &sv) {
        return decodeString(sv, true);
    }

    static const char *const tokenNames[];

    static const char *toString(Token tk) {
        return tokenNames[static_cast<size_t>(tk)];
    }
};

class AVRO_DECL JsonNullFormatter {
public:
    explicit JsonNullFormatter(StreamWriter &) {}

    void handleObjectStart() {}
    void handleObjectEnd() {}
    void handleValueEnd() {}
    void handleColon() {}
};

class AVRO_DECL JsonPrettyFormatter {
    StreamWriter &out_;
    size_t level_;
    std::vector<uint8_t> indent_;

    static const int CHARS_PER_LEVEL = 2;

    void printIndent() {
        size_t charsToIndent = level_ * CHARS_PER_LEVEL;
        if (indent_.size() < charsToIndent) {
            indent_.resize(charsToIndent * 2, ' ');
        }
        out_.writeBytes(indent_.data(), charsToIndent);
    }

public:
    explicit JsonPrettyFormatter(StreamWriter &out) : out_(out), level_(0), indent_(10, ' ') {}

    void handleObjectStart() {
        out_.write('\n');
        ++level_;
        printIndent();
    }

    void handleObjectEnd() {
        out_.write('\n');
        --level_;
        printIndent();
    }

    void handleValueEnd() {
        out_.write('\n');
        printIndent();
    }

    void handleColon() {
        out_.write(' ');
    }
};

template<class F>
class AVRO_DECL JsonGenerator {
    StreamWriter out_;
    F formatter_;
    enum State {
        stStart,
        stArray0,
        stArrayN,
        stMap0,
        stMapN,
        stKey,
    };

    std::stack<State> stateStack;
    State top;

    void write(const char *b, const char *p) {
        if (b != p) {
            out_.writeBytes(reinterpret_cast<const uint8_t *>(b), p - b);
        }
    }

    void escape(char c, const char *b, const char *p) {
        write(b, p);
        out_.write('\\');
        out_.write(c);
    }

    void escapeCtl(char c) {
        escapeUnicode(static_cast<uint8_t>(c));
    }

    void writeHex(char c) {
        out_.write(toHex((static_cast<unsigned char>(c)) / 16));
        out_.write(toHex((static_cast<unsigned char>(c)) % 16));
    }

    void escapeUnicode16(uint32_t c) {
        out_.write('\\');
        out_.write('u');
        writeHex(static_cast<char>((c >> 8) & 0xff));
        writeHex(static_cast<char>(c & 0xff));
    }
    void escapeUnicode(uint32_t c) {
        if (c < 0x10000) {
            escapeUnicode16(c);
        } else if (c < 0x110000) {
            c -= 0x10000;
            escapeUnicode16(((c >> 10) & 0x3ff) | 0xd800);
            escapeUnicode16((c & 0x3ff) | 0xdc00);
        } else {
            throw Exception("Invalid code-point: {}", c);
        }
    }
    void doEncodeString(const char *b, size_t len, bool binary) {
        const char *e = b + len;
        out_.write('"');
        for (const char *p = b; p != e; p++) {
            if ((*p & 0x80) != 0) {
                write(b, p);
                if (binary) {
                    escapeCtl(*p);
                } else if ((*p & 0x40) == 0) {
                    throw Exception("Invalid UTF-8 sequence");
                } else {
                    int more = 1;
                    uint32_t value;
                    if ((*p & 0x20) != 0) {
                        more++;
                        if ((*p & 0x10) != 0) {
                            more++;
                            if ((*p & 0x08) != 0) {
                                throw Exception("Invalid UTF-8 sequence");
                            } else {
                                value = *p & 0x07;
                            }
                        } else {
                            value = *p & 0x0f;
                        }
                    } else {
                        value = *p & 0x1f;
                    }
                    for (int i = 0; i < more; ++i) {
                        if (++p == e || (*p & 0xc0) != 0x80) {
                            throw Exception("Invalid UTF-8 sequence");
                        }
                        value <<= 6;
                        value |= *p & 0x3f;
                    }
                    escapeUnicode(value);
                }
            } else {
                switch (*p) {
                    case '\\':
                    case '"':
                        escape(*p, b, p);
                        break;
                    case '\b':
                        escape('b', b, p);
                        break;
                    case '\f':
                        escape('f', b, p);
                        break;
                    case '\n':
                        escape('n', b, p);
                        break;
                    case '\r':
                        escape('r', b, p);
                        break;
                    case '\t':
                        escape('t', b, p);
                        break;
                    default:
                        if (std::iscntrl(*p, std::locale::classic())) {
                            write(b, p);
                            escapeCtl(*p);
                            break;
                        } else {
                            continue;
                        }
                }
            }
            b = p + 1;
        }
        write(b, e);
        out_.write('"');
    }

    void sep() {
        if (top == stArrayN) {
            out_.write(',');
            formatter_.handleValueEnd();
        } else if (top == stArray0) {
            top = stArrayN;
        }
    }

    void sep2() {
        if (top == stKey) {
            top = stMapN;
        }
    }

public:
    JsonGenerator() : formatter_(out_), top(stStart) {}

    void init(OutputStream &os) {
        out_.reset(os);
    }

    void flush() {
        out_.flush();
    }

    int64_t byteCount() const {
        return out_.byteCount();
    }

    void encodeNull() {
        sep();
        out_.writeBytes(reinterpret_cast<const uint8_t *>("null"), 4);
        sep2();
    }

    void encodeBool(bool b) {
        sep();
        if (b) {
            out_.writeBytes(reinterpret_cast<const uint8_t *>("true"), 4);
        } else {
            out_.writeBytes(reinterpret_cast<const uint8_t *>("false"), 5);
        }
        sep2();
    }

    template<typename T>
    void encodeNumber(T t) {
        sep();
        std::ostringstream oss;
        oss << boost::lexical_cast<std::string>(t);
        const std::string s = oss.str();
        out_.writeBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        sep2();
    }

    void encodeNumber(double t) {
        sep();
        std::ostringstream oss;
        if (boost::math::isfinite(t)) {
            oss << boost::lexical_cast<std::string>(t);
        } else if (boost::math::isnan(t)) {
            oss << "NaN";
        } else if (t == std::numeric_limits<double>::infinity()) {
            oss << "Infinity";
        } else {
            oss << "-Infinity";
        }
        const std::string s = oss.str();
        out_.writeBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        sep2();
    }

    void encodeString(const std::string &s) {
        if (top == stMap0) {
            top = stKey;
        } else if (top == stMapN) {
            out_.write(',');
            formatter_.handleValueEnd();
            top = stKey;
        } else if (top == stKey) {
            top = stMapN;
        } else {
            sep();
        }
        doEncodeString(s.c_str(), s.size(), false);
        if (top == stKey) {
            out_.write(':');
            formatter_.handleColon();
        }
    }

    void encodeBinary(const uint8_t *bytes, size_t len) {
        sep();
        doEncodeString(reinterpret_cast<const char *>(bytes), len, true);
        sep2();
    }

    void arrayStart() {
        sep();
        stateStack.push(top);
        top = stArray0;
        out_.write('[');
        formatter_.handleObjectStart();
    }

    void arrayEnd() {
        top = stateStack.top();
        stateStack.pop();
        formatter_.handleObjectEnd();
        out_.write(']');
        sep2();
    }

    void objectStart() {
        sep();
        stateStack.push(top);
        top = stMap0;
        out_.write('{');
        formatter_.handleObjectStart();
    }

    void objectEnd() {
        top = stateStack.top();
        stateStack.pop();
        formatter_.handleObjectEnd();
        out_.write('}');
        sep2();
    }
};

} // namespace json
} // namespace avro

#endif

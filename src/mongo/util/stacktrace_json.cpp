/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/stacktrace_json.h"

#include <cctype>

#include "mongo/bson/bsonobj.h"
#include "mongo/util/assert_util.h"

namespace mongo::stacktrace_detail {
namespace {
constexpr StringData kHexDigits = "0123456789ABCDEF"_sd;

/**
 * Wrapper that streams a string-like object to a StackTraceSink, surrounded by double
 * quotes.
 */
template <typename T>
class Quoted {
public:
    explicit Quoted(const T& v) : _v(v) {}

    friend StackTraceSink& operator<<(StackTraceSink& sink, const Quoted& q) {
        return sink << kQuote << q._v << kQuote;
    }

private:
    static constexpr StringData kQuote = "\""_sd;
    const T& _v;
};
}  // namespace

StringData Hex::toHex(uint64_t x, Buf& buf) {
    char* data = buf.data();
    size_t nBuf = buf.size();
    char* p = data + nBuf;
    if (!x) {
        *--p = '0';
    } else {
        for (int d = 0; d < 16; ++d) {
            if (!x)
                break;
            *--p = kHexDigits[x & 0xf];
            x >>= 4;
        }
    }
    return StringData(p, data + nBuf - p);
}

uint64_t Hex::fromHex(StringData s) {
    uint64_t x = 0;
    for (char c : s) {
        char uc = std::toupper(static_cast<unsigned char>(c));
        if (size_t pos = kHexDigits.find(uc); pos == std::string::npos) {
            return x;
        } else {
            x <<= 4;
            x += pos;
        }
    }
    return x;
}

CheapJson::Value::Value(CheapJson* env, Kind k) : _env(env), _kind(k) {
    if (_kind == kObj) {
        _env->_sink << "{";
    } else if (_kind == kArr) {
        _env->_sink << "[";
    }
}

CheapJson::Value::~Value() {
    if (_kind == kObj) {
        _env->_sink << "}";
    } else if (_kind == kArr) {
        _env->_sink << "]";
    }
}

auto CheapJson::Value::appendObj() -> Value {
    _next();
    return Value{_env, kObj};
}

auto CheapJson::Value::appendArr() -> Value {
    _next();
    return Value{_env, kArr};
}

auto CheapJson::Value::appendKey(StringData k) -> Value {
    fassert(_kind == kObj, "appendKey requires this to be kObj");
    _next();
    _env->_sink << Quoted(k) << ":";
    return Value{_env, kNop};
}

void CheapJson::Value::append(StringData v) {
    _next();
    _env->_sink << Quoted(v);
}

void CheapJson::Value::append(uint64_t v) {
    _next();
    _env->_sink << v;
}

void CheapJson::Value::append(const BSONElement& be) {
    if (_kind == kObj)
        appendKey(be.fieldNameStringData())._copyBsonElementValue(be);
    else
        _copyBsonElementValue(be);
}

void CheapJson::Value::_copyBsonElementValue(const BSONElement& be) {
    switch (be.type()) {
        case BSONType::String:
            append(be.valueStringData());
            break;
        case BSONType::NumberInt:
            append(be.Int());
            break;
        case BSONType::Object: {
            Value sub = appendObj();
            for (const BSONElement& e : be.Obj())
                sub.append(e);
        } break;
        case BSONType::Array: {
            Value sub = appendArr();
            for (const BSONElement& e : be.Array())
                sub.append(e);
        } break;
        default:
            // warning() << "unknown type " << be.type() << "\n";
            break;
    }
}

void CheapJson::Value::_next() {
    _env->_sink << _sep;
    _sep = ","_sd;
}

auto CheapJson::doc() -> Value {
    return Value(this);
}

CheapJson::CheapJson(StackTraceSink& sink) : _sink(sink) {}

}  // namespace mongo::stacktrace_detail

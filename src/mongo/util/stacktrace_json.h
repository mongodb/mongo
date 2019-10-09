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

#pragma once

#include <array>
#include <ostream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/stacktrace.h"

namespace mongo::stacktrace_detail {

/**
 * A utility for uint64_t <=> uppercase hex string conversions. It
 * can be used to produce a StringData.
 *
 *     sink << Hex(x).str();  // as a temporary
 *
 *     Hex hx(x);
 *     StringData sd = hx.str()  // sd storage is in `hx`.
 */
class Hex {
public:
    using Buf = std::array<char, 16>;

    static StringData toHex(uint64_t x, Buf& buf);

    static uint64_t fromHex(StringData s);

    explicit Hex(uint64_t x) : _str(toHex(x, _buf)) {}

    StringData str() const {
        return _str;
    }

private:
    Buf _buf;
    StringData _str;
};

/** An append-only, async-safe, malloc-free Json emitter. */
class CheapJson {
public:
    class Value;

    explicit CheapJson(StackTraceSink& sink);

    // Create an empty JSON document.
    Value doc();

private:
    StackTraceSink& _sink;
};

/**
 * A Json Value node being emitted. Emits {}/[] braces, keyval ":" separators, commas,
 * and quotes.  To use this, make a Value for the root document and call `append*`
 * members, adding a nested structure of objects, arrays, and scalars to the active
 * Value.
 *
 * The constructor emits any appropriate opening brace, and the destructor emits any
 * appropriate closing brace. Keeps an internal state so that a comma is emitted on the
 * second and subsequent append call.
 */
class CheapJson::Value {
public:
    /** The empty root document, which emits no braces. */
    explicit Value(CheapJson* env) : Value(env, kNop) {}

    /** Emit the closing brace if any. */
    ~Value();

    /** Begin a Json Array. Returns a Value that can be used to append elements to it. */
    Value appendArr();

    /** Begin a Json Object. Returns a Value that can be used to append elements to it. */
    Value appendObj();

    /** Append key `k` to this Json Object. Returns the empty Value mapped to `k`. */
    Value appendKey(StringData k);

    /** Append string `v`, surrounded by doublequote characters. */
    void append(StringData v);

    /** Append integer `v`, in decimal. */
    void append(uint64_t v);

    /**
     * Append the elements of `be` to this Object or Array.
     * Behavior depends on the kind of Value this is.
     *   - If object: append `be` keys and values.
     *   - If array: append `be` values only.
     */
    void append(const BSONElement& be);

private:
    enum Kind {
        kNop,  // A blank Value, not an aggregate, emits no punctuation. Can emit one element.
        kObj,  // Object: can emit multiple key-value pairs: {k1:v1, k2:v2, ...}
        kArr,  // Array: can emit multiple scalars [v1, v2, ...]
    };

    /* Emit the opening brace corresponding to the specified `k`. */
    Value(CheapJson* env, Kind k);
    void _copyBsonElementValue(const BSONElement& be);
    void _next();

    CheapJson* _env;
    Kind _kind;
    StringData _sep;  // Emitted upon append. Starts empty, then set to ",".
};

}  // namespace mongo::stacktrace_detail

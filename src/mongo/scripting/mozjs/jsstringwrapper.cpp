/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/scripting/mozjs/jsstringwrapper.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/util/assert_util.h"

#include <cstring>

#include <fmt/format.h>
#include <js/CharacterEncoding.h>
#include <js/String.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

JSStringWrapper::JSStringWrapper(std::int32_t value) : _isSet(true) {
    auto formatted = fmt::format_int(value);
    _length = formatted.size();
    strncpy(_buf, formatted.c_str(), sizeof(_buf) - 1);
    _buf[sizeof(_buf) - 1] = '\0';
}

JSStringWrapper::JSStringWrapper(JSContext* cx, JSString* str) : _isSet(true) {
    if (!str)
        throwCurrentJSException(cx, ErrorCodes::InternalError, "Cannot encode null JSString");

    // We have to do this flatstring thing because no public api tells us
    // how long the utf8 strings we get out are.
    //
    // Well, at least js/CharacterEncoding's GetDeflatedUTF8StringLength
    // and StringToLinearString are all in the public headers...
    JSLinearString* flat = JS::StringToLinearString(cx, str);
    if (!flat)
        throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to flatten JSString");

    _length = JS::GetDeflatedUTF8StringLength(flat);

    char* out;
    if (_length < sizeof(_buf)) {
        out = _buf;
    } else {
        _str.reset(new char[_length + 1]);
        out = _str.get();
    }

    JS::DeflateStringToUTF8Buffer(flat, mozilla::Span(out, _length));
    out[_length] = '\0';
}

StringData JSStringWrapper::toStringData() const {
    invariant(_isSet);
    return StringData(_str ? _str.get() : _buf, _length);
}

std::string JSStringWrapper::toString() const {
    return std::string{toStringData()};
}

JSStringWrapper::operator bool() const {
    return _isSet;
}

}  // namespace mozjs
}  // namespace mongo

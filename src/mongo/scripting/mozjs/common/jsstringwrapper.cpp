// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/jsstringwrapper.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/exception.h"
#include "mongo/util/assert_util.h"

#include <cstring>
#include <string_view>

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

std::string_view JSStringWrapper::toStringData() const {
    invariant(_isSet);
    return std::string_view(_str ? _str.get() : _buf, _length);
}

std::string JSStringWrapper::toString() const {
    return std::string{toStringData()};
}

JSStringWrapper::operator bool() const {
    return _isSet;
}

}  // namespace mozjs
}  // namespace mongo

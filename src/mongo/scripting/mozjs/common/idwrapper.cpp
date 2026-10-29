// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/idwrapper.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/exception.h"
#include "mongo/scripting/mozjs/common/jsstringwrapper.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <js/Id.h>
#include <js/RootingAPI.h>
#include <js/String.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

IdWrapper::IdWrapper(JSContext* cx, JS::HandleId value) : _context(cx), _value(cx, value) {}

std::string IdWrapper::toString() const {
    JSStringWrapper jsstr;
    return std::string{toStringData(&jsstr)};
}

std::string_view IdWrapper::toStringData(JSStringWrapper* jsstr) const {
    if (_value.isString()) {
        *jsstr = JSStringWrapper(_context, _value.toString());
    } else if (_value.isInt()) {
        *jsstr = JSStringWrapper(_value.toInt());
    } else {
        throwCurrentJSException(_context,
                                ErrorCodes::TypeMismatch,
                                "Cannot toString() non-string and non-integer jsid");
    }

    return jsstr->toStringData();
}

uint32_t IdWrapper::toInt32() const {
    uassert(ErrorCodes::TypeMismatch, "Cannot toInt32() non-integer jsid", _value.isInt());

    return _value.toInt();
}

void IdWrapper::toValue(JS::MutableHandleValue value) const {
    if (isInt()) {
        value.setInt32(toInt32());
        return;
    }

    if (isString()) {
        auto str = _value.toString();
        value.setString(str);
        return;
    }

    uasserted(ErrorCodes::BadValue, "Failed to toValue() non-string and non-integer jsid");
}

bool IdWrapper::equals(std::string_view sd) const {
    return sd.compare(toString()) == 0;
}

bool IdWrapper::equalsAscii(std::string_view sd) const {
    if (isString()) {
        auto str = _value.toString();

        if (!str) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to id.toString()");
        }

        bool matched;
        if (!JS_StringEqualsAscii(_context, str, std::string{sd}.c_str(), &matched)) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_StringEqualsAscii");
        }

        return matched;
    }

    if (isInt()) {
        JSStringWrapper jsstr(toInt32());
        return jsstr.toStringData().compare(sd) == 0;
    }

    uasserted(ErrorCodes::BadValue, "Cannot equalsAscii non-string non-integer jsid");
}

bool IdWrapper::isInt() const {
    return _value.isInt();
}

bool IdWrapper::isString() const {
    return _value.isString();
}

}  // namespace mozjs
}  // namespace mongo

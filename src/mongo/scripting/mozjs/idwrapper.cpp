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

#include "mongo/scripting/mozjs/idwrapper.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/util/assert_util.h"

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

StringData IdWrapper::toStringData(JSStringWrapper* jsstr) const {
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

bool IdWrapper::equals(StringData sd) const {
    return sd.compare(toString()) == 0;
}

bool IdWrapper::equalsAscii(StringData sd) const {
    if (isString()) {
        auto str = _value.toString();

        if (!str) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to id.toString()");
        }

        bool matched;
        if (!JS_StringEqualsAscii(_context, str, sd.data(), &matched)) {
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

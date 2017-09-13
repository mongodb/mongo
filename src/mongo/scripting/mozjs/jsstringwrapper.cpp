/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/jsstringwrapper.h"

#include <js/CharacterEncoding.h>
#include <jsapi.h>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace mozjs {

JSStringWrapper::JSStringWrapper(std::int32_t value) : _isSet(true) {
    _length = sprintf(_buf, "%d", value);
}

JSStringWrapper::JSStringWrapper(JSContext* cx, JSString* str) : _isSet(true) {
    if (!str)
        throwCurrentJSException(cx, ErrorCodes::InternalError, "Cannot encode null JSString");

    // We have to do this flatstring thing because no public api tells us
    // how long the utf8 strings we get out are.
    //
    // Well, at least js/CharacterEncoding's GetDeflatedUTF8StringLength
    // and JS_flattenString are all in the public headers...
    JSFlatString* flat = JS_FlattenString(cx, str);
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

    JS::DeflateStringToUTF8Buffer(flat, mozilla::RangedPtr<char>(out, _length));
    out[_length] = '\0';
}

StringData JSStringWrapper::toStringData() const {
    invariant(_isSet);
    return StringData(_str ? _str.get() : _buf, _length);
}

std::string JSStringWrapper::toString() const {
    return toStringData().toString();
}

JSStringWrapper::operator bool() const {
    return _isSet;
}

}  // namespace mozjs
}  // namespace mongo

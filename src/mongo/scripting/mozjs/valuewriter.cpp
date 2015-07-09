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

#include "mongo/scripting/mozjs/valuewriter.h"

#include <js/Conversions.h>

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/util/base64.h"

namespace mongo {
namespace mozjs {

ValueWriter::ValueWriter(JSContext* cx, JS::HandleValue value, int depth)
    : _context(cx), _value(value), _depth(depth), _originalParent(nullptr) {}

void ValueWriter::setOriginalBSON(BSONObj* obj) {
    _originalParent = obj;
}

int ValueWriter::type() {
    if (_value.isNull())
        return jstNULL;
    if (_value.isUndefined())
        return Undefined;
    if (_value.isString())
        return String;
    if (JS_IsArrayObject(_context, _value))
        return Array;
    if (_value.isBoolean())
        return Bool;

    // We could do something more sophisticated here by checking to see if we
    // round trip through int32_t, int64_t and double and picking a type that
    // way, for now just always come back as double for numbers though (it's
    // what we did for v8)
    if (_value.isNumber())
        return NumberDouble;

    if (_value.isObject()) {
        JS::RootedObject obj(_context, _value.toObjectOrNull());
        if (JS_ObjectIsDate(_context, obj))
            return Date;
        if (JS_ObjectIsFunction(_context, obj))
            return Code;

        return Object;
    }

    uasserted(ErrorCodes::BadValue, "unable to get type");
}

BSONObj ValueWriter::toBSON() {
    if (!_value.isObject())
        return BSONObj();

    JS::RootedObject obj(_context, _value.toObjectOrNull());

    if (getScope(_context)->getBsonProto().instanceOf(obj)) {
        BSONObj* originalBSON;
        bool altered;

        std::tie(originalBSON, altered) = BSONInfo::originalBSON(_context, obj);

        if (!altered)
            return *originalBSON;
    }

    BSONObjBuilder bob;
    ObjectWrapper(_context, obj, _depth).writeThis(&bob);

    return bob.obj();
}

std::string ValueWriter::toString() {
    return JSStringWrapper(_context, JS::ToString(_context, _value)).toString();
}

double ValueWriter::toNumber() {
    double out;
    if (JS::ToNumber(_context, _value, &out))
        return out;

    throwCurrentJSException(_context, ErrorCodes::BadValue, "Failure to convert value to number");
}

bool ValueWriter::toBoolean() {
    return JS::ToBoolean(_value);
}

int32_t ValueWriter::toInt32() {
    int32_t out;
    if (JS::ToInt32(_context, _value, &out))
        return out;

    throwCurrentJSException(_context, ErrorCodes::BadValue, "Failure to convert value to number");
}

int64_t ValueWriter::toInt64() {
    int64_t out;
    if (JS::ToInt64(_context, _value, &out))
        return out;

    throwCurrentJSException(_context, ErrorCodes::BadValue, "Failure to convert value to number");
}

void ValueWriter::writeThis(BSONObjBuilder* b, StringData sd) {
    uassert(17279,
            str::stream() << "Exceeded depth limit of " << 150
                          << " when converting js object to BSON. Do you have a cycle?",
            _depth < 149);

    // Null char should be at the end, not in the string
    uassert(16985,
            str::stream() << "JavaScript property (name) contains a null char "
                          << "which is not allowed in BSON. "
                          << (_originalParent ? _originalParent->jsonString() : ""),
            (std::string::npos == sd.find('\0')));

    if (_value.isString()) {
        b->append(sd, toString());
    } else if (_value.isNumber()) {
        double val = toNumber();

        // if previous type was integer, keep it
        int intval = static_cast<int>(val);

        if (val == intval && _originalParent) {
            // This makes copying an object of numbers O(n**2) :(
            BSONElement elmt = _originalParent->getField(sd);
            if (elmt.type() == mongo::NumberInt) {
                b->append(sd, intval);
                return;
            }
        }

        b->append(sd, val);
    } else if (_value.isObject()) {
        JS::RootedObject childObj(_context, _value.toObjectOrNull());
        _writeObject(b, sd, childObj);
    } else if (_value.isBoolean()) {
        b->appendBool(sd, _value.toBoolean());
    } else if (_value.isUndefined()) {
        b->appendUndefined(sd);
    } else if (_value.isNull()) {
        b->appendNull(sd);
    } else {
        uasserted(16662,
                  str::stream() << "unable to convert JavaScript property to mongo element " << sd);
    }
}

void ValueWriter::_writeObject(BSONObjBuilder* b, StringData sd, JS::HandleObject obj) {
    auto scope = getScope(_context);

    ObjectWrapper o(_context, obj, _depth);

    if (JS_ObjectIsFunction(_context, _value.toObjectOrNull())) {
        uassert(16716,
                "cannot convert native function to BSON",
                !scope->getNativeFunctionProto().instanceOf(obj));
        b->appendCode(sd, ValueWriter(_context, _value).toString());
    } else if (JS_ObjectIsRegExp(_context, obj)) {
        JS::RootedValue v(_context);
        v.setObjectOrNull(obj);

        std::string regex = ValueWriter(_context, v).toString();
        regex = regex.substr(1);
        std::string r = regex.substr(0, regex.rfind('/'));
        std::string o = regex.substr(regex.rfind('/') + 1);

        b->appendRegex(sd, r, o);
    } else if (JS_ObjectIsDate(_context, obj)) {
        JS::RootedValue dateval(_context);
        o.callMethod("getTime", &dateval);

        auto d = Date_t::fromMillisSinceEpoch(ValueWriter(_context, dateval).toNumber());
        b->appendDate(sd, d);
    } else if (scope->getOidProto().instanceOf(obj)) {
        b->append(sd, OID(o.getString("str")));
    } else if (scope->getNumberLongProto().instanceOf(obj)) {
        long long out = NumberLongInfo::ToNumberLong(_context, obj);
        b->append(sd, out);
    } else if (scope->getNumberIntProto().instanceOf(obj)) {
        b->append(sd, NumberIntInfo::ToNumberInt(_context, obj));
    } else if (scope->getDbPointerProto().instanceOf(obj)) {
        JS::RootedValue id(_context);
        o.getValue("id", &id);

        b->appendDBRef(sd, o.getString("ns"), OID(ObjectWrapper(_context, id).getString("str")));
    } else if (scope->getBinDataProto().instanceOf(obj)) {
        auto str = static_cast<std::string*>(JS_GetPrivate(obj));

        auto binData = base64::decode(*str);

        b->appendBinData(sd,
                         binData.size(),
                         static_cast<mongo::BinDataType>(static_cast<int>(o.getNumber("type"))),
                         binData.c_str());
    } else if (scope->getTimestampProto().instanceOf(obj)) {
        Timestamp ot(o.getNumber("t"), o.getNumber("i"));
        b->append(sd, ot);
    } else if (scope->getMinKeyProto().instanceOf(obj)) {
        b->appendMinKey(sd);
    } else if (scope->getMaxKeyProto().instanceOf(obj)) {
        b->appendMaxKey(sd);
    } else {
        // nested object or array

        BSONObjBuilder subbob(JS_IsArrayObject(_context, obj) ? b->subarrayStart(sd)
                                                              : b->subobjStart(sd));

        ObjectWrapper child(_context, obj, _depth + 1);

        child.writeThis(b);
    }
}

}  // namespace mozjs
}  // namespace mongo

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

ValueWriter::ValueWriter(JSContext* cx, JS::HandleValue value)
    : _context(cx), _value(value), _originalParent(nullptr) {}

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

std::string ValueWriter::typeAsString() {
    if (_value.isNull())
        return "null";
    if (_value.isUndefined())
        return "undefined";
    if (_value.isString())
        return "string";
    if (JS_IsArrayObject(_context, _value))
        return "array";
    if (_value.isBoolean())
        return "boolean";
    if (_value.isNumber())
        return "number";

    if (_value.isObject()) {
        JS::RootedObject obj(_context, _value.toObjectOrNull());
        if (JS_IsArrayObject(_context, obj))
            return "array";
        if (JS_ObjectIsDate(_context, obj))
            return "date";
        if (JS_ObjectIsFunction(_context, obj))
            return "function";

        return ObjectWrapper(_context, _value).getClassName();
    }

    uasserted(ErrorCodes::BadValue, "unable to get type");
}

BSONObj ValueWriter::toBSON() {
    if (!_value.isObject())
        return BSONObj();

    JS::RootedObject obj(_context, _value.toObjectOrNull());

    return ObjectWrapper(_context, obj).toBSON();
}

std::string ValueWriter::toString() {
    JSStringWrapper jsstr;
    return toStringData(&jsstr).toString();
}

StringData ValueWriter::toStringData(JSStringWrapper* jsstr) {
    *jsstr = JSStringWrapper(_context, JS::ToString(_context, _value));
    return jsstr->toStringData();
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
    if (getScope(_context)->getProto<NumberLongInfo>().instanceOf(_value))
        return NumberLongInfo::ToNumberLong(_context, _value);

    if (JS::ToInt64(_context, _value, &out))
        return out;

    throwCurrentJSException(_context, ErrorCodes::BadValue, "Failure to convert value to number");
}

Decimal128 ValueWriter::toDecimal128() {
    if (_value.isNumber()) {
        return Decimal128(toNumber());
    }

    if (getScope(_context)->getProto<NumberIntInfo>().instanceOf(_value))
        return Decimal128(NumberIntInfo::ToNumberInt(_context, _value));

    if (getScope(_context)->getProto<NumberLongInfo>().instanceOf(_value))
        return Decimal128(static_cast<int64_t>(NumberLongInfo::ToNumberLong(_context, _value)));

    if (getScope(_context)->getProto<NumberDecimalInfo>().instanceOf(_value))
        return NumberDecimalInfo::ToNumberDecimal(_context, _value);

    if (_value.isString()) {
        return Decimal128(toString());
    }

    uasserted(ErrorCodes::BadValue, str::stream() << "Unable to write Decimal128 value.");
}

void ValueWriter::writeThis(BSONObjBuilder* b,
                            StringData sd,
                            ObjectWrapper::WriteFieldRecursionFrames* frames) {
    uassert(17279,
            str::stream() << "Exceeded depth limit of " << ObjectWrapper::kMaxWriteFieldDepth
                          << " when converting js object to BSON. Do you have a cycle?",
            frames->size() < ObjectWrapper::kMaxWriteFieldDepth);

    // Null char should be at the end, not in the string
    uassert(16985,
            str::stream() << "JavaScript property (name) contains a null char "
                          << "which is not allowed in BSON. "
                          << (_originalParent ? _originalParent->jsonString() : ""),
            (std::string::npos == sd.find('\0')));

    if (_value.isString()) {
        JSStringWrapper jsstr;
        b->append(sd, toStringData(&jsstr));
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
        _writeObject(b, sd, frames);
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

void ValueWriter::_writeObject(BSONObjBuilder* b,
                               StringData sd,
                               ObjectWrapper::WriteFieldRecursionFrames* frames) {
    auto scope = getScope(_context);

    // We open a block here because it's important that the two rooting types
    // we need (obj and o) go out of scope before we actually open a
    // new WriteFieldFrame (in the emplace at the bottom of the function). If
    // we don't do this, we'll destroy the local roots in this function body
    // before the frame we added, which will break the gc rooting list.
    {
        JS::RootedObject obj(_context, _value.toObjectOrNull());
        ObjectWrapper o(_context, obj);

        auto jsclass = JS_GetClass(obj);

        if (jsclass) {
            if (scope->getProto<OIDInfo>().getJSClass() == jsclass) {
                b->append(sd, OIDInfo::getOID(_context, obj));

                return;
            }

            if (scope->getProto<NumberLongInfo>().getJSClass() == jsclass) {
                long long out = NumberLongInfo::ToNumberLong(_context, obj);
                b->append(sd, out);

                return;
            }

            if (scope->getProto<NumberIntInfo>().getJSClass() == jsclass) {
                b->append(sd, NumberIntInfo::ToNumberInt(_context, obj));

                return;
            }

            if (scope->getProto<NumberDecimalInfo>().getJSClass() == jsclass) {
                b->append(sd, NumberDecimalInfo::ToNumberDecimal(_context, obj));

                return;
            }

            if (scope->getProto<DBPointerInfo>().getJSClass() == jsclass) {
                JS::RootedValue id(_context);
                o.getValue("id", &id);

                b->appendDBRef(sd, o.getString("ns"), OIDInfo::getOID(_context, id));

                return;
            }

            if (scope->getProto<BinDataInfo>().getJSClass() == jsclass) {
                auto str = static_cast<std::string*>(JS_GetPrivate(obj));

                auto binData = base64::decode(*str);

                b->appendBinData(sd,
                                 binData.size(),
                                 static_cast<mongo::BinDataType>(
                                     static_cast<int>(o.getNumber(InternedString::type))),
                                 binData.c_str());

                return;
            }

            if (scope->getProto<TimestampInfo>().getJSClass() == jsclass) {
                Timestamp ot(o.getNumber("t"), o.getNumber("i"));
                b->append(sd, ot);

                return;
            }

            if (scope->getProto<MinKeyInfo>().getJSClass() == jsclass) {
                b->appendMinKey(sd);

                return;
            }

            if (scope->getProto<MaxKeyInfo>().getJSClass() == jsclass) {
                b->appendMaxKey(sd);

                return;
            }
        }

        auto protoKey = JS::IdentifyStandardInstance(obj);

        switch (protoKey) {
            case JSProto_Function: {
                uassert(16716,
                        "cannot convert native function to BSON",
                        !scope->getProto<NativeFunctionInfo>().instanceOf(obj));
                JSStringWrapper jsstr;
                b->appendCode(sd, ValueWriter(_context, _value).toStringData(&jsstr));
                return;
            }
            case JSProto_RegExp: {
                JS::RootedValue v(_context);
                v.setObjectOrNull(obj);

                std::string regex = ValueWriter(_context, v).toString();
                regex = regex.substr(1);
                std::string r = regex.substr(0, regex.rfind('/'));
                std::string o = regex.substr(regex.rfind('/') + 1);

                b->appendRegex(sd, r, o);

                return;
            }
            case JSProto_Date: {
                JS::RootedValue dateval(_context);
                o.callMethod("getTime", &dateval);

                auto d = Date_t::fromMillisSinceEpoch(ValueWriter(_context, dateval).toNumber());
                b->appendDate(sd, d);

                return;
            }
            default:
                break;
        }
    }

    // nested object or array

    // This emplace is effectively a recursive function call, as this code path
    // unwinds back to ObjectWrapper::toBSON. In that function we'll actually
    // write the child we've just pushed onto the frames stack.
    frames->emplace(_context, _value.toObjectOrNull(), b, sd);
}

}  // namespace mozjs
}  // namespace mongo

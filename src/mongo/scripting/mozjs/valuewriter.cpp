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

#include "mongo/scripting/mozjs/valuewriter.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/bindata.h"
#include "mongo/scripting/mozjs/code.h"
#include "mongo/scripting/mozjs/dbpointer.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/maxkey.h"
#include "mongo/scripting/mozjs/minkey.h"
#include "mongo/scripting/mozjs/nativefunction.h"
#include "mongo/scripting/mozjs/numberdecimal.h"
#include "mongo/scripting/mozjs/numberint.h"
#include "mongo/scripting/mozjs/numberlong.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/oid.h"
#include "mongo/scripting/mozjs/timestamp.h"
#include "mongo/scripting/mozjs/wraptype.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <new>

#include <jsapi.h>
#include <jsfriendapi.h>
#include <jspubtd.h>

#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>
#include <js/Array.h>
#include <js/ComparisonOperators.h>
#include <js/Conversions.h>
#include <js/Date.h>
#include <js/Object.h>
#include <js/RegExp.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

ValueWriter::ValueWriter(JSContext* cx, JS::HandleValue value)
    : _context(cx), _value(value), _originalParent(nullptr) {}

void ValueWriter::setOriginalBSON(BSONObj* obj) {
    _originalParent = obj;
}

int ValueWriter::type() {
    if (_value.isNull())
        return stdx::to_underlying(BSONType::null);
    if (_value.isUndefined())
        return stdx::to_underlying(BSONType::undefined);
    if (_value.isString())
        return stdx::to_underlying(BSONType::string);

    bool isArray;

    if (!JS::IsArrayObject(_context, _value, &isArray)) {
        uasserted(ErrorCodes::BadValue, "unable to check if type is an array");
    }
    if (isArray) {
        return stdx::to_underlying(BSONType::array);
    }

    if (_value.isBoolean()) {
        return stdx::to_underlying(BSONType::boolean);
    }

    // We could do something more sophisticated here by checking to see if we
    // round trip through int32_t, int64_t and double and picking a type that
    // way, for now just always come back as double for numbers though (it's
    // what we did for v8)
    if (_value.isNumber()) {
        return stdx::to_underlying(BSONType::numberDouble);
    }

    if (_value.isObject()) {
        JS::RootedObject obj(_context, _value.toObjectOrNull());

        bool isDate;
        if (!JS::ObjectIsDate(_context, obj, &isDate)) {
            uasserted(ErrorCodes::BadValue, "unable to check if type is a date");
        }
        if (isDate) {
            return stdx::to_underlying(BSONType::date);
        }

        bool isRegExp;
        if (!JS::ObjectIsRegExp(_context, obj, &isRegExp)) {
            uasserted(ErrorCodes::BadValue, "unable to check if type is a regexp");
        }
        if (isRegExp) {
            return stdx::to_underlying(BSONType::regEx);
        }

        if (js::IsFunctionObject(obj)) {
            return stdx::to_underlying(BSONType::code);
        }

        if (auto jsClass = JS::GetClass(obj)) {
            auto scope = getScope(_context);
            if (scope->getProto<NumberIntInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::numberInt);
            } else if (scope->getProto<NumberLongInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::numberLong);
            } else if (scope->getProto<NumberDecimalInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::numberDecimal);
            } else if (scope->getProto<OIDInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::oid);
            } else if (scope->getProto<BinDataInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::binData);
            } else if (scope->getProto<TimestampInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::timestamp);
            } else if (scope->getProto<MinKeyInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::minKey);
            } else if (scope->getProto<MaxKeyInfo>().getJSClass() == jsClass) {
                return stdx::to_underlying(BSONType::maxKey);
            }
        }

        return stdx::to_underlying(BSONType::object);
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

    bool isArray;

    if (!JS::IsArrayObject(_context, _value, &isArray)) {
        uasserted(ErrorCodes::BadValue, "unable to check if type is an array");
    }

    if (isArray)
        return "array";
    if (_value.isBoolean())
        return "boolean";
    if (_value.isNumber())
        return "number";

    if (_value.isObject()) {
        JS::RootedObject obj(_context, _value.toObjectOrNull());

        if (!JS::IsArrayObject(_context, obj, &isArray)) {
            uasserted(ErrorCodes::BadValue, "unable to check if type is an array");
        }
        if (isArray)
            return "array";

        bool isDate;

        if (!JS::ObjectIsDate(_context, obj, &isDate)) {
            uasserted(ErrorCodes::BadValue, "unable to check if type is a date");
        }
        if (isDate)
            return "date";

        if (js::IsFunctionObject(obj))
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
    return std::string{toStringData(&jsstr)};
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
    std::uint32_t signalingFlags = 0;
    if (_value.isNumber()) {
        return Decimal128(toNumber(), Decimal128::kRoundTo15Digits);
    }
    if (getScope(_context)->getProto<NumberIntInfo>().instanceOf(_value))
        return Decimal128(NumberIntInfo::ToNumberInt(_context, _value));

    if (getScope(_context)->getProto<NumberLongInfo>().instanceOf(_value))
        return Decimal128(static_cast<int64_t>(NumberLongInfo::ToNumberLong(_context, _value)));

    if (getScope(_context)->getProto<NumberDecimalInfo>().instanceOf(_value))
        return NumberDecimalInfo::ToNumberDecimal(_context, _value);

    if (_value.isString()) {
        std::string input = toString();
        Decimal128 decimal = Decimal128(input, &signalingFlags);
        uassert(ErrorCodes::BadValue,
                str::stream() << "Input is not a valid Decimal128 value.",
                !Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kInvalid));
        uassert(ErrorCodes::BadValue,
                str::stream() << "Input out of range of Decimal128 value (inexact).",
                !Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kInexact));
        uassert(ErrorCodes::BadValue,
                str::stream() << "Input out of range of Decimal128 value (underflow).",
                !Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kUnderflow));
        uassert(ErrorCodes::BadValue,
                str::stream() << "Input out of range of Decimal128 value (overflow).",
                !Decimal128::hasFlag(signalingFlags, Decimal128::SignalingFlag::kOverflow));

        return decimal;
    }
    uasserted(ErrorCodes::BadValue, str::stream() << "Unable to write Decimal128 value.");
}

OID ValueWriter::toOID() {
    if (getScope(_context)->getProto<OIDInfo>().instanceOf(_value)) {
        return OIDInfo::getOID(_context, _value);
    }

    throwCurrentJSException(_context, ErrorCodes::BadValue, "Unable to write ObjectId value.");
}

void ValueWriter::toBinData(std::function<void(const BSONBinData&)> withBinData) {
    if (!getScope(_context)->getProto<BinDataInfo>().instanceOf(_value)) {
        throwCurrentJSException(_context, ErrorCodes::BadValue, "Unable to write BinData value.");
    }

    JS::RootedObject obj(_context, _value.toObjectOrNull());
    ObjectWrapper wrapper(_context, obj);
    auto subType = wrapper.getNumber(InternedString::type);
    uassert(6123400, "BinData sub type must be between 0 and 255", subType >= 0 && subType <= 255);

    auto binDataStr =
        JS::GetMaybePtrFromReservedSlot<std::string>(obj, BinDataInfo::BinDataStringSlot);
    uassert(ErrorCodes::BadValue, "Cannot call getter on BinData prototype", binDataStr);

    auto binData = base64::decode(*binDataStr);
    withBinData(BSONBinData(binData.c_str(),
                            binData.size(),
                            static_cast<mongo::BinDataType>(static_cast<int>(subType))));
}

Timestamp ValueWriter::toTimestamp() {
    JS::RootedObject obj(_context, _value.toObjectOrNull());

    uassert(ErrorCodes::BadValue,
            "Unable to write Timestamp value.",
            getScope(_context)->getProto<TimestampInfo>().getJSClass() == JS::GetClass(obj));

    return TimestampInfo::getValidatedValue(_context, obj);
}

JSRegEx ValueWriter::toRegEx() {
    std::string regexStr = toString();
    uassert(6123401, "Empty regular expression", regexStr.size() > 0);
    uassert(6123402, "Invalid regular expression", regexStr[0] == '/');

    return JSRegEx(regexStr.substr(1, regexStr.rfind('/')),
                   regexStr.substr(regexStr.rfind('/') + 1));
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

        // if previous type was integer, attempt to represent 'val' as an integer
        auto intval = representAs<int>(val);

        if (intval && _originalParent) {
            // This makes copying an object of numbers O(n**2) :(
            BSONElement elmt = _originalParent->getField(sd);
            if (elmt.type() == BSONType::numberInt) {
                b->append(sd, *intval);
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

        auto jsclass = JS::GetClass(obj);

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

            if (scope->getProto<CodeInfo>().getJSClass() == jsclass) {
                if (o.hasOwnField(InternedString::scope)  // CodeWScope
                    && o.type(InternedString::scope) == stdx::to_underlying(BSONType::object)) {
                    if (o.type(InternedString::code) != stdx::to_underlying(BSONType::string)) {
                        uasserted(ErrorCodes::BadValue, "code must be a string");
                    }

                    b->appendCodeWScope(
                        sd, o.getString(InternedString::code), o.getObject(InternedString::scope));
                } else {  // Code
                    if (o.type(InternedString::code) != stdx::to_underlying(BSONType::string)) {
                        uasserted(ErrorCodes::BadValue, "code must be a string");
                    }

                    b->appendCode(sd, o.getString(InternedString::code));
                }
                return;
            }

            if (scope->getProto<NumberDecimalInfo>().getJSClass() == jsclass) {
                b->append(sd, NumberDecimalInfo::ToNumberDecimal(_context, obj));

                return;
            }

            if (scope->getProto<DBPointerInfo>().getJSClass() == jsclass) {
                uassert(ErrorCodes::BadValue,
                        "can't serialize DBPointer prototype",
                        scope->getProto<DBPointerInfo>().getProto() != obj);

                JS::RootedValue id(_context);
                o.getValue("id", &id);

                b->appendDBRef(sd, o.getString("ns"), OIDInfo::getOID(_context, id));

                return;
            }

            if (scope->getProto<BinDataInfo>().getJSClass() == jsclass) {
                auto str = JS::GetMaybePtrFromReservedSlot<std::string>(
                    obj, BinDataInfo::BinDataStringSlot);

                uassert(ErrorCodes::BadValue, "Cannot call getter on BinData prototype", str);

                auto binData = base64::decode(*str);

                auto subType = o.getNumber(InternedString::type);
                uassert(5677700,
                        "BinData sub type must be between 0 and 255",
                        subType >= 0 && subType <= 255);

                b->appendBinData(sd,
                                 binData.size(),
                                 static_cast<mongo::BinDataType>(static_cast<int>(subType)),
                                 binData.c_str());

                return;
            }

            if (scope->getProto<TimestampInfo>().getJSClass() == jsclass) {
                Timestamp ot = TimestampInfo::getValidatedValue(_context, obj);
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

        auto protoKey = JS::IdentifyStandardInstanceOrPrototype(obj);

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
                Date_t d;
                if (JS::IdentifyStandardPrototype(obj) == JSProto_Date) {
                    d = Date_t::fromMillisSinceEpoch(0);
                } else {
                    JS::RootedValue dateval(_context);
                    o.callMethod("getTime", &dateval);
                    d = Date_t::fromMillisSinceEpoch(ValueWriter(_context, dateval).toInt64());
                }

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

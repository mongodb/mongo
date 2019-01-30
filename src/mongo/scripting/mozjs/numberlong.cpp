
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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/numberlong.h"

#include <boost/optional.hpp>
#include <js/Conversions.h>

#include "mongo/base/parse_number.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/text.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec NumberLongInfo::methods[6] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toNumber, NumberLongInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NumberLongInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, NumberLongInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(valueOf, NumberLongInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(compare, NumberLongInfo),
    JS_FS_END};

const char* const NumberLongInfo::className = "NumberLong";

void NumberLongInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto numLong = static_cast<int64_t*>(JS_GetPrivate(obj));

    if (numLong)
        getScope(fop)->trackedDelete(numLong);
}

int64_t NumberLongInfo::ToNumberLong(JSContext* cx, JS::HandleValue thisv) {
    auto numLong = static_cast<int64_t*>(JS_GetPrivate(thisv.toObjectOrNull()));
    return numLong ? *numLong : 0;
}

int64_t NumberLongInfo::ToNumberLong(JSContext* cx, JS::HandleObject thisv) {
    auto numLong = static_cast<int64_t*>(JS_GetPrivate(thisv));
    return numLong ? *numLong : 0;
}

void NumberLongInfo::Functions::valueOf::call(JSContext* cx, JS::CallArgs args) {
    int64_t out = NumberLongInfo::ToNumberLong(cx, args.thisv());
    ValueReader(cx, args.rval()).fromDouble(out);
}

void NumberLongInfo::Functions::toNumber::call(JSContext* cx, JS::CallArgs args) {
    valueOf::call(cx, args);
}

void NumberLongInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    str::stream ss;

    int64_t val = NumberLongInfo::ToNumberLong(cx, args.thisv());

    const int64_t limit = 2LL << 30;

    if (val <= -limit || limit <= val)
        ss << "NumberLong(\"" << val << "\")";
    else
        ss << "NumberLong(" << val << ")";


    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}

void NumberLongInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    int64_t val = NumberLongInfo::ToNumberLong(cx, args.thisv());

    ValueReader(cx, args.rval())
        .fromBSON(BSON("$numberLong" << std::to_string(val)), nullptr, false);
}

void NumberLongInfo::Functions::compare::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "NumberLong.compare() needs 1 argument", args.length() == 1);
    uassert(ErrorCodes::BadValue,
            "NumberLong.compare() argument must be a NumberLong",
            getScope(cx)->getProto<NumberLongInfo>().instanceOf(args.get(0)));

    int64_t thisVal = NumberLongInfo::ToNumberLong(cx, args.thisv());
    int64_t otherVal = NumberLongInfo::ToNumberLong(cx, args.get(0));

    int comparison = 0;
    if (thisVal < otherVal) {
        comparison = -1;
    } else if (thisVal > otherVal) {
        comparison = 1;
    }

    ValueReader(cx, args.rval()).fromDouble(comparison);
}

void NumberLongInfo::Functions::floatApprox::call(JSContext* cx, JS::CallArgs args) {
    int64_t numLong = NumberLongInfo::ToNumberLong(cx, args.thisv());
    ValueReader(cx, args.rval()).fromDouble(numLong);
}

void NumberLongInfo::Functions::top::call(JSContext* cx, JS::CallArgs args) {
    auto numULong = static_cast<uint64_t>(NumberLongInfo::ToNumberLong(cx, args.thisv()));
    ValueReader(cx, args.rval()).fromDouble(numULong >> 32);
}

void NumberLongInfo::Functions::bottom::call(JSContext* cx, JS::CallArgs args) {
    auto numULong = static_cast<uint64_t>(NumberLongInfo::ToNumberLong(cx, args.thisv()));
    ValueReader(cx, args.rval()).fromDouble(numULong & 0x00000000FFFFFFFF);
}

void NumberLongInfo::construct(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue,
            "NumberLong needs 0, 1 or 3 arguments",
            args.length() == 0 || args.length() == 1 || args.length() == 3);

    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);

    scope->getProto<NumberLongInfo>().newObject(&thisv);

    int64_t numLong;

    ObjectWrapper o(cx, thisv);

    if (args.length() == 0) {
        numLong = 0;
    } else if (args.length() == 1) {
        auto arg = args.get(0);

        if (arg.isInt32()) {
            numLong = arg.toInt32();
        } else if (arg.isDouble()) {
            auto opt = representAs<int64_t>(arg.toDouble());
            uassert(ErrorCodes::BadValue,
                    "number passed to NumberLong must be representable as an int64_t",
                    opt);
            numLong = *opt;
        } else if (arg.isString()) {
            // For string values we call strtoll because we expect non-number string
            // values to fail rather than return 0 (which is the behavior of ToInt64).
            std::string str = ValueWriter(cx, arg).toString();

            // Call parseNumberFromStringWithBase() function to convert string to a number
            Status status = parseNumberFromStringWithBase(str, 10, &numLong);
            uassert(ErrorCodes::BadValue, "could not convert string to long long", status.isOK());
        } else {
            numLong = ValueWriter(cx, arg).toInt64();
        }
    } else {
        uassert(ErrorCodes::BadValue, "floatApprox must be a number", args.get(0).isNumber());
        uassert(ErrorCodes::BadValue, "top must be a number", args.get(1).isNumber());
        uassert(ErrorCodes::BadValue, "bottom must be a number", args.get(2).isNumber());

        auto topOpt = representAs<uint32_t>(args.get(1).toNumber());
        uassert(ErrorCodes::BadValue, "top must be a 32 bit unsigned number", topOpt);
        uint64_t top = *topOpt;

        auto botOpt = representAs<uint32_t>(args.get(2).toNumber());
        uassert(ErrorCodes::BadValue, "bottom must be a 32 bit unsigned number", botOpt);
        uint64_t bot = *botOpt;

        numLong = (top << 32) + bot;
    }

    JS_SetPrivate(thisv, scope->trackedNew<int64_t>(numLong));

    args.rval().setObjectOrNull(thisv);
}

void NumberLongInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    JS::RootedValue undef(cx);
    undef.setUndefined();

    // floatapprox
    if (!JS_DefinePropertyById(
            cx,
            proto,
            getScope(cx)->getInternedStringId(InternedString::floatApprox),
            smUtils::wrapConstrainedMethod<Functions::floatApprox, false, NumberLongInfo>,
            nullptr,
            JSPROP_ENUMERATE)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefinePropertyById");
    }

    // top
    if (!JS_DefinePropertyById(
            cx,
            proto,
            getScope(cx)->getInternedStringId(InternedString::top),
            smUtils::wrapConstrainedMethod<Functions::top, false, NumberLongInfo>,
            nullptr,
            JSPROP_ENUMERATE)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefinePropertyById");
    }

    // bottom
    if (!JS_DefinePropertyById(
            cx,
            proto,
            getScope(cx)->getInternedStringId(InternedString::bottom),
            smUtils::wrapConstrainedMethod<Functions::bottom, false, NumberLongInfo>,
            nullptr,
            JSPROP_ENUMERATE)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefinePropertyById");
    }
}

}  // namespace mozjs
}  // namespace mongo

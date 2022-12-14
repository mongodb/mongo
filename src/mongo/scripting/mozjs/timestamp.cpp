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

#include "mongo/scripting/mozjs/timestamp.h"

#include <limits>
#include <string>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec TimestampInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, TimestampInfo),
    JS_FS_END,
};

const char* const TimestampInfo::className = "Timestamp";

namespace {
// Checks that a JavaScript value is a number in the range of an unsigned 32-bit integer. The 'name'
// is used to describe the parameter that failed validation in the error message to the user.
double getTimestampComponent(JSContext* cx, JS::HandleValue component, std::string name) {
    int64_t maxValue = std::numeric_limits<uint32_t>::max();
    if (!component.isNumber())
        uasserted(ErrorCodes::BadValue, str::stream() << name << " must be a number");
    int64_t val = ValueWriter(cx, component).toInt64();
    if (val < 0 || val > maxValue) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << name << " must be non-negative and not greater than " << maxValue
                                << ", got " << val);
    }
    return val;
}
}  // namespace

void TimestampInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);
    scope->getProto<TimestampInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    if (args.length() == 0) {
        o.setNumber(InternedString::t, 0);
        o.setNumber(InternedString::i, 0);
    } else if (args.length() == 2) {
        o.setNumber(InternedString::t,
                    getTimestampComponent(cx, args.get(0), "Timestamp time (seconds)"));
        o.setNumber(InternedString::i,
                    getTimestampComponent(cx, args.get(1), "Timestamp increment"));
    } else {
        uasserted(ErrorCodes::BadValue, "Timestamp needs 0 or 2 arguments");
    }

    args.rval().setObjectOrNull(thisv);
}

Timestamp TimestampInfo::getValidatedValue(JSContext* cx, JS::HandleObject obj) {
    ObjectWrapper wrapper(cx, obj);

    uassert(6900900,
            "Malformed timestamp in JavaScript: missing timestamp field, 't'",
            wrapper.hasOwnField("t"));
    uassert(6900901,
            "Malformed timestamp in JavaScript: missing increment field, 'i'",
            wrapper.hasOwnField("i"));

    JS::RootedValue time(cx);
    wrapper.getValue("t", &time);

    JS::RootedValue increment(cx);
    wrapper.getValue("i", &increment);

    return {static_cast<uint32_t>(getTimestampComponent(cx, time, "Timestamp time (seconds)")),
            static_cast<uint32_t>(getTimestampComponent(cx, increment, "Timestamp increment"))};
}

void TimestampInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    ValueReader(cx, args.rval())
        .fromBSON(BSON("$timestamp" << BSON("t" << o.getNumber(InternedString::t) << "i"
                                                << o.getNumber(InternedString::i))),
                  nullptr,
                  false);
}

}  // namespace mozjs
}  // namespace mongo

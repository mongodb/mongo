// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/timestamp.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <limits>
#include <string>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

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
    auto* runtime = getCommonRuntime(cx);

    JS::RootedObject thisv(cx);
    getProto<TimestampInfo>(runtime).newObject(&thisv);
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

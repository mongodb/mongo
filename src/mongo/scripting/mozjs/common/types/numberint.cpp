// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/numberint.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/freeOpToJSContext.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <string>

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec NumberIntInfo::methods[5] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toNumber, NumberIntInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NumberIntInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, NumberIntInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(valueOf, NumberIntInfo),
    JS_FS_END,
};

const char* const NumberIntInfo::className = "NumberInt";

void NumberIntInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto x = JS::GetMaybePtrFromReservedSlot<int>(obj, IntSlot);

    if (x)
        trackedDelete(getCommonRuntime(freeOpToJSContext(gcCtx)), x);
}

int NumberIntInfo::ToNumberInt(JSContext* cx, JS::HandleValue thisv) {
    auto x = JS::GetMaybePtrFromReservedSlot<int>(thisv.toObjectOrNull(), IntSlot);

    return x ? *x : 0;
}

int NumberIntInfo::ToNumberInt(JSContext* cx, JS::HandleObject thisv) {
    auto x = JS::GetMaybePtrFromReservedSlot<int>(thisv, IntSlot);

    return x ? *x : 0;
}

void NumberIntInfo::Functions::valueOf::call(JSContext* cx, JS::CallArgs args) {
    int out = NumberIntInfo::ToNumberInt(cx, args.thisv());

    args.rval().setInt32(out);
}

void NumberIntInfo::Functions::toNumber::call(JSContext* cx, JS::CallArgs args) {
    valueOf::call(cx, args);
}

void NumberIntInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    int val = NumberIntInfo::ToNumberInt(cx, args.thisv());

    str::stream ss;
    ss << "NumberInt(" << val << ")";

    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}

void NumberIntInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    int val = NumberIntInfo::ToNumberInt(cx, args.thisv());

    args.rval().setInt32(val);
}

void NumberIntInfo::construct(JSContext* cx, JS::CallArgs args) {
    JS::RootedObject thisv(cx);

    getCommonRuntime(cx)->numberIntProto().newObject(&thisv);

    int32_t x = 0;

    if (args.length() == 0) {
        // Do nothing
    } else if (args.length() == 1) {
        x = ValueWriter(cx, args.get(0)).toInt32();
    } else {
        uasserted(ErrorCodes::BadValue, "NumberInt takes 0 or 1 arguments");
    }
    JS::SetReservedSlot(thisv, IntSlot, JS::PrivateValue(trackedNew<int>(getCommonRuntime(cx), x)));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo

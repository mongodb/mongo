// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/numberdecimal.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/common/freeOpToJSContext.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <string>

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec NumberDecimalInfo::methods[3] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NumberDecimalInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, NumberDecimalInfo),
    JS_FS_END,
};

const char* const NumberDecimalInfo::className = "NumberDecimal";

void NumberDecimalInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto x = JS::GetMaybePtrFromReservedSlot<Decimal128>(obj, Decimal128Slot);

    if (x)
        trackedDelete(getCommonRuntime(freeOpToJSContext(gcCtx)), x);
}

Decimal128 NumberDecimalInfo::ToNumberDecimal(JSContext* cx, JS::HandleValue thisv) {
    auto x = JS::GetMaybePtrFromReservedSlot<Decimal128>(thisv.toObjectOrNull(), Decimal128Slot);

    return x ? *x : Decimal128(0);
}

Decimal128 NumberDecimalInfo::ToNumberDecimal(JSContext* cx, JS::HandleObject thisv) {
    auto x = JS::GetMaybePtrFromReservedSlot<Decimal128>(thisv, Decimal128Slot);

    return x ? *x : Decimal128(0);
}

void NumberDecimalInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    Decimal128 val = NumberDecimalInfo::ToNumberDecimal(cx, args.thisv());

    str::stream ss;
    ss << "NumberDecimal(\"" << val.toString() << "\")";

    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}

void NumberDecimalInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    Decimal128 val = NumberDecimalInfo::ToNumberDecimal(cx, args.thisv());

    ValueReader(cx, args.rval()).fromBSON(BSON("$numberDecimal" << val.toString()), nullptr, false);
}

void NumberDecimalInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto* runtime = getCommonRuntime(cx);

    JS::RootedObject thisv(cx);

    getProto<NumberDecimalInfo>(runtime).newObject(&thisv);

    Decimal128 x(0);

    if (args.length() == 0) {
        // Do nothing
    } else if (args.length() == 1) {
        x = ValueWriter(cx, args.get(0)).toDecimal128();
    } else {
        uasserted(ErrorCodes::BadValue, "NumberDecimal takes 0 or 1 arguments");
    }
    JS::SetReservedSlot(
        thisv, Decimal128Slot, JS::PrivateValue(trackedNew<Decimal128>(runtime, x)));

    args.rval().setObjectOrNull(thisv);
}

void NumberDecimalInfo::make(JSContext* cx, JS::MutableHandleValue thisv, Decimal128 decimal) {
    auto* runtime = getCommonRuntime(cx);

    getProto<NumberDecimalInfo>(runtime).newObject(thisv);
    JS::SetReservedSlot(thisv.toObjectOrNull(),
                        Decimal128Slot,
                        JS::PrivateValue(trackedNew<Decimal128>(runtime, decimal)));
}

}  // namespace mozjs
}  // namespace mongo

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

#include "mongo/scripting/mozjs/numberdecimal.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep
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
        getScope(gcCtx)->trackedDelete(x);
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
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);

    scope->getProto<NumberDecimalInfo>().newObject(&thisv);

    Decimal128 x(0);

    if (args.length() == 0) {
        // Do nothing
    } else if (args.length() == 1) {
        x = ValueWriter(cx, args.get(0)).toDecimal128();
    } else {
        uasserted(ErrorCodes::BadValue, "NumberDecimal takes 0 or 1 arguments");
    }
    JS::SetReservedSlot(thisv, Decimal128Slot, JS::PrivateValue(scope->trackedNew<Decimal128>(x)));

    args.rval().setObjectOrNull(thisv);
}

void NumberDecimalInfo::make(JSContext* cx, JS::MutableHandleValue thisv, Decimal128 decimal) {
    auto scope = getScope(cx);

    scope->getProto<NumberDecimalInfo>().newObject(thisv);
    JS::SetReservedSlot(thisv.toObjectOrNull(),
                        Decimal128Slot,
                        JS::PrivateValue(scope->trackedNew<Decimal128>(decimal)));
}

}  // namespace mozjs
}  // namespace mongo

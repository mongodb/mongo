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

#include "mongo/scripting/mozjs/numberint.h"

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec NumberIntInfo::methods[4] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toNumber, NumberIntInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, NumberIntInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(valueOf, NumberIntInfo),
    JS_FS_END,
};

const char* const NumberIntInfo::className = "NumberInt";

void NumberIntInfo::finalize(JSFreeOp* fop, JSObject* obj) {
    auto x = static_cast<int*>(JS_GetPrivate(obj));

    if (x)
        delete x;
}

int NumberIntInfo::ToNumberInt(JSContext* cx, JS::HandleValue thisv) {
    auto x = static_cast<int*>(JS_GetPrivate(thisv.toObjectOrNull()));

    return x ? *x : 0;
}

int NumberIntInfo::ToNumberInt(JSContext* cx, JS::HandleObject thisv) {
    auto x = static_cast<int*>(JS_GetPrivate(thisv));

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

void NumberIntInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);

    scope->getProto<NumberIntInfo>().newObject(&thisv);

    int32_t x = 0;

    if (args.length() == 0) {
        // Do nothing
    } else if (args.length() == 1) {
        x = ValueWriter(cx, args.get(0)).toInt32();
    } else {
        uasserted(ErrorCodes::BadValue, "NumberInt takes 0 or 1 arguments");
    }

    JS_SetPrivate(thisv, new int(x));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo

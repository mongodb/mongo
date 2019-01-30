
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

#include "mongo/scripting/mozjs/status.h"

#include "mongo/scripting/jsexception.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"

namespace mongo {
namespace mozjs {

const char* const MongoStatusInfo::className = "MongoStatus";
const char* const MongoStatusInfo::inheritFrom = "Error";

Status MongoStatusInfo::toStatus(JSContext* cx, JS::HandleObject object) {
    return *static_cast<Status*>(JS_GetPrivate(object));
}

Status MongoStatusInfo::toStatus(JSContext* cx, JS::HandleValue value) {
    return *static_cast<Status*>(JS_GetPrivate(value.toObjectOrNull()));
}

void MongoStatusInfo::fromStatus(JSContext* cx, Status status, JS::MutableHandleValue value) {
    auto scope = getScope(cx);

    JS::RootedValue undef(cx);
    undef.setUndefined();

    JS::AutoValueArray<1> args(cx);
    ValueReader(cx, args[0]).fromStringData(status.reason());
    JS::RootedObject error(cx);
    scope->getProto<ErrorInfo>().newInstance(args, &error);

    JS::RootedObject thisv(cx);
    scope->getProto<MongoStatusInfo>().newObjectWithProto(&thisv, error);
    ObjectWrapper thisvObj(cx, thisv);
    thisvObj.defineProperty(InternedString::code,
                            JSPROP_ENUMERATE,
                            smUtils::wrapConstrainedMethod<Functions::code, false, MongoStatusInfo>,
                            nullptr);

    thisvObj.defineProperty(
        InternedString::reason,
        JSPROP_ENUMERATE,
        smUtils::wrapConstrainedMethod<Functions::reason, false, MongoStatusInfo>,
        nullptr);

    // We intentionally omit JSPROP_ENUMERATE to match how Error.prototype.stack is a non-enumerable
    // property.
    thisvObj.defineProperty(
        InternedString::stack,
        0,
        smUtils::wrapConstrainedMethod<Functions::stack, false, MongoStatusInfo>,
        nullptr);

    JS_SetPrivate(thisv, scope->trackedNew<Status>(std::move(status)));

    value.setObjectOrNull(thisv);
}

void MongoStatusInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto code = args.get(0).toInt32();
    auto reason = JSStringWrapper(cx, args.get(1).toString()).toString();

    JS::RootedValue out(cx);
    fromStatus(cx, Status(ErrorCodes::Error(code), std::move(reason)), &out);

    args.rval().set(out);
}

void MongoStatusInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto status = static_cast<Status*>(JS_GetPrivate(obj));

    if (status)
        getScope(fop)->trackedDelete(status);
}

void MongoStatusInfo::Functions::code::call(JSContext* cx, JS::CallArgs args) {
    args.rval().setInt32(toStatus(cx, args.thisv()).code());
}

void MongoStatusInfo::Functions::reason::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromStringData(toStatus(cx, args.thisv()).reason());
}

void MongoStatusInfo::Functions::stack::call(JSContext* cx, JS::CallArgs args) {
    JS::RootedObject thisv(cx, args.thisv().toObjectOrNull());
    JS::RootedObject parent(cx);

    if (!JS_GetPrototype(cx, thisv, &parent)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Couldn't get prototype");
    }

    ObjectWrapper parentWrapper(cx, parent);

    auto status = toStatus(cx, args.thisv());
    if (auto extraInfo = status.extraInfo<JSExceptionInfo>()) {
        // 'status' represents an uncaught JavaScript exception that was handled in C++. It is
        // expected to have been thrown by a JSThread. We chain its stacktrace together with the
        // stacktrace of the JavaScript exception in the current thread in order to show more
        // context about the latter's cause.
        JS::RootedValue stack(cx);
        ValueReader(cx, &stack)
            .fromStringData(extraInfo->stack + parentWrapper.getString(InternedString::stack));

        // We redefine the "stack" property as the combined JavaScript stacktrace. It is important
        // that we omit (TODO/FIXME, no more JSPROP_SHARED) JSPROP_SHARED to the
        // thisvObj.defineProperty() call in order to have
        // SpiderMonkey allocate memory for the string value. We also intentionally omit
        // JSPROP_ENUMERATE to match how Error.prototype.stack is a non-enumerable property.
        ObjectWrapper thisvObj(cx, args.thisv());
        thisvObj.defineProperty(InternedString::stack, stack, 0U);

        // We intentionally use thisvObj.getValue() to access the "stack" property to implicitly
        // verify it has been redefined correctly, as the alternative would be infinite recursion.
        thisvObj.getValue(InternedString::stack, args.rval());
    } else {
        parentWrapper.getValue(InternedString::stack, args.rval());
    }
}

void MongoStatusInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    auto scope = getScope(cx);

    JS_SetPrivate(proto, scope->trackedNew<Status>(Status::OK()));
}

}  // namespace mozjs
}  // namespace mongo

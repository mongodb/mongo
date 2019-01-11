
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

#include "mongo/scripting/mozjs/oid.h"

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec OIDInfo::methods[3] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toString, OIDInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toJSON, OIDInfo),
    JS_FS_END,
};

const char* const OIDInfo::className = "ObjectId";

void OIDInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto oid = static_cast<OID*>(JS_GetPrivate(obj));

    if (oid) {
        getScope(fop)->trackedDelete(oid);
    }
}

void OIDInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    auto oid = static_cast<OID*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    std::string str = str::stream() << "ObjectId(\"" << oid->toString() << "\")";

    ValueReader(cx, args.rval()).fromStringData(str);
}

void OIDInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    auto oid = static_cast<OID*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    ValueReader(cx, args.rval()).fromBSON(BSON("$oid" << oid->toString()), nullptr, false);
}

void OIDInfo::Functions::getter::call(JSContext* cx, JS::CallArgs args) {
    auto oid = static_cast<OID*>(JS_GetPrivate(args.thisv().toObjectOrNull()));

    ValueReader(cx, args.rval()).fromStringData(oid->toString());
}

void OIDInfo::construct(JSContext* cx, JS::CallArgs args) {
    OID oid;
    if (args.length() == 0) {
        oid.init();
    } else {
        auto str = ValueWriter(cx, args.get(0)).toString();

        Scope::validateObjectIdString(str);
        oid.init(str);
    }

    make(cx, oid, args.rval());
}

void OIDInfo::make(JSContext* cx, const OID& oid, JS::MutableHandleValue out) {
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);
    scope->getProto<OIDInfo>().newObject(&thisv);
    JS_SetPrivate(thisv, scope->trackedNew<OID>(oid));

    out.setObjectOrNull(thisv);
}

OID OIDInfo::getOID(JSContext* cx, JS::HandleValue value) {
    JS::RootedObject obj(cx, value.toObjectOrNull());
    return getOID(cx, obj);
}

OID OIDInfo::getOID(JSContext* cx, JS::HandleObject object) {
    auto oid = static_cast<OID*>(JS_GetPrivate(object));

    if (oid) {
        return *oid;
    }

    uasserted(ErrorCodes::BadValue, "Can't call getOID on OID prototype");
}

void OIDInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    JS::RootedValue undef(cx);
    undef.setUndefined();

    if (!JS_DefinePropertyById(cx,
                               proto,
                               getScope(cx)->getInternedStringId(InternedString::str),
                               smUtils::wrapConstrainedMethod<Functions::getter, true, OIDInfo>,
                               nullptr,
                               JSPROP_ENUMERATE)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefinePropertyById");
    }
}

}  // namespace mozjs
}  // namespace mongo

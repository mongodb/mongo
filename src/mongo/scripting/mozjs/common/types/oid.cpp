// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/oid.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/common/freeOpToJSContext.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/oid_validation.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <jsapi.h>

#include <js/CallArgs.h>
#include <js/ComparisonOperators.h>
#include <js/Object.h>
#include <js/PropertyDescriptor.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec OIDInfo::methods[3] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toString, OIDInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toJSON, OIDInfo),
    JS_FS_END,
};

const char* const OIDInfo::className = "ObjectId";

void OIDInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto oid = JS::GetMaybePtrFromReservedSlot<OID>(obj, OIDSlot);

    if (oid) {
        trackedDelete(getCommonRuntime(freeOpToJSContext(gcCtx)), oid);
    }
}

void OIDInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    auto oid = JS::GetMaybePtrFromReservedSlot<OID>(args.thisv().toObjectOrNull(), OIDSlot);

    std::string str = str::stream() << "ObjectId(\"" << oid->toString() << "\")";

    ValueReader(cx, args.rval()).fromStringData(str);
}

void OIDInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    auto oid = JS::GetMaybePtrFromReservedSlot<OID>(args.thisv().toObjectOrNull(), OIDSlot);

    ValueReader(cx, args.rval()).fromBSON(BSON("$oid" << oid->toString()), nullptr, false);
}

void OIDInfo::Functions::getter::call(JSContext* cx, JS::CallArgs args) {
    auto oid = JS::GetMaybePtrFromReservedSlot<OID>(args.thisv().toObjectOrNull(), OIDSlot);

    ValueReader(cx, args.rval()).fromStringData(oid->toString());
}

void OIDInfo::construct(JSContext* cx, JS::CallArgs args) {
    OID oid;
    if (args.length() == 0) {
        oid.init();
    } else {
        auto str = ValueWriter(cx, args.get(0)).toString();

        validateObjectIdString(str);
        oid.init(str);
    }

    make(cx, oid, args.rval());
}

void OIDInfo::make(JSContext* cx, const OID& oid, JS::MutableHandleValue out) {
    auto* runtime = getCommonRuntime(cx);

    JS::RootedObject thisv(cx);
    getProto<OIDInfo>(runtime).newObject(&thisv);
    JS::SetReservedSlot(thisv, OIDSlot, JS::PrivateValue(trackedNew<OID>(runtime, oid)));

    out.setObjectOrNull(thisv);
}

OID OIDInfo::getOID(JSContext* cx, JS::HandleValue value) {
    JS::RootedObject obj(cx, value.toObjectOrNull());
    return getOID(cx, obj);
}

OID OIDInfo::getOID(JSContext* cx, JS::HandleObject object) {
    auto oid = JS::GetMaybePtrFromReservedSlot<OID>(object, OIDSlot);

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
                               getCommonRuntime(cx)->getInternedStringId(InternedString::str),
                               smUtils::wrapConstrainedMethod<Functions::getter, true, OIDInfo>,
                               nullptr,
                               JSPROP_ENUMERATE)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefinePropertyById");
    }
}

}  // namespace mozjs
}  // namespace mongo

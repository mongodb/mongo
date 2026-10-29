// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/maxkey.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/Value.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec MaxKeyInfo::methods[4] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(tojson, MaxKeyInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, MaxKeyInfo),
    MONGO_ATTACH_JS_FUNCTION_SYM_WITH_FLAGS(hasInstance, hasInstance),
    JS_FS_END,
};

const char* const MaxKeyInfo::className = "MaxKey";

void MaxKeyInfo::construct(JSContext* cx, JS::CallArgs args) {
    call(cx, args);
}

/**
 * The idea here is that MinKey and MaxKey are singleton callable objects that
 * return the singleton when called. This enables all instances to compare
 * == and === to MinKey even if created by "new MinKey()" in JS.
 */
void MaxKeyInfo::call(JSContext* cx, JS::CallArgs args) {
    auto* runtime = getCommonRuntime(cx);

    ObjectWrapper o(cx, getProto<MaxKeyInfo>(runtime).getProto());

    JS::RootedValue val(cx);

    if (!o.hasField(InternedString::singleton)) {
        JS::RootedObject thisv(cx);
        getProto<MaxKeyInfo>(runtime).newObject(&thisv);

        val.setObjectOrNull(thisv);
        o.setValue(InternedString::singleton, val);
    } else {
        o.getValue(InternedString::singleton, &val);

        if (!getProto<MaxKeyInfo>(runtime).instanceOf(val))
            uasserted(ErrorCodes::BadValue, "MaxKey singleton not of type MaxKey");
    }

    args.rval().set(val);
}

void MaxKeyInfo::Functions::tojson::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromStringData("{ \"$maxKey\" : 1 }");
}

void MaxKeyInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromBSON(BSON("$maxKey" << 1), nullptr, false);
}

void MaxKeyInfo::Functions::hasInstance::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "hasInstance needs 1 argument", args.length() == 1);
    uassert(ErrorCodes::BadValue, "argument must be an object", args.get(0).isObject());

    auto* runtime = getCommonRuntime(cx);
    args.rval().setBoolean(getProto<MaxKeyInfo>(runtime).instanceOf(args.get(0)));
}

void MaxKeyInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    ObjectWrapper protoWrapper(cx, proto);

    JS::RootedValue value(cx);
    getCommonRuntime(cx)->maxKeyProto().newObject(&value);

    ObjectWrapper(cx, global).setValue(InternedString::MaxKey, value);
    protoWrapper.setValue(InternedString::singleton, value);
}

}  // namespace mozjs
}  // namespace mongo

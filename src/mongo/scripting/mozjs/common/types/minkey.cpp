// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/minkey.h"

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

const JSFunctionSpec MinKeyInfo::methods[4] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(tojson, MinKeyInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toJSON, MinKeyInfo),
    MONGO_ATTACH_JS_FUNCTION_SYM_WITH_FLAGS(hasInstance, hasInstance),
    JS_FS_END,
};

const char* const MinKeyInfo::className = "MinKey";

void MinKeyInfo::construct(JSContext* cx, JS::CallArgs args) {
    call(cx, args);
}

/**
 * The idea here is that MinKey and MaxKey are singleton callable objects that
 * return the singleton when called. This enables all instances to compare
 * == and === to MinKey even if created by "new MinKey()" in JS.
 */
void MinKeyInfo::call(JSContext* cx, JS::CallArgs args) {
    auto* runtime = getCommonRuntime(cx);

    ObjectWrapper o(cx, getProto<MinKeyInfo>(runtime).getProto());

    JS::RootedValue val(cx);

    if (!o.hasField(InternedString::singleton)) {
        JS::RootedObject thisv(cx);
        getProto<MinKeyInfo>(runtime).newObject(&thisv);

        val.setObjectOrNull(thisv);
        o.setValue(InternedString::singleton, val);
    } else {
        o.getValue(InternedString::singleton, &val);

        if (!getProto<MinKeyInfo>(runtime).instanceOf(val))
            uasserted(ErrorCodes::BadValue, "MinKey singleton not of type MinKey");
    }

    args.rval().set(val);
}


void MinKeyInfo::Functions::tojson::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromStringData("{ \"$minKey\" : 1 }");
}

void MinKeyInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromBSON(BSON("$minKey" << 1), nullptr, false);
}

void MinKeyInfo::Functions::hasInstance::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "hasInstance needs 1 argument", args.length() == 1);
    uassert(ErrorCodes::BadValue, "argument must be an object", args.get(0).isObject());

    auto* runtime = getCommonRuntime(cx);
    args.rval().setBoolean(getProto<MinKeyInfo>(runtime).instanceOf(args.get(0)));
}

void MinKeyInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    ObjectWrapper protoWrapper(cx, proto);

    JS::RootedValue value(cx);
    getCommonRuntime(cx)->minKeyProto().newObject(&value);

    ObjectWrapper(cx, global).setValue(InternedString::MinKey, value);
    protoWrapper.setValue(InternedString::singleton, value);
}

}  // namespace mozjs
}  // namespace mongo

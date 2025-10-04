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

#include "mongo/scripting/mozjs/maxkey.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep
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
    auto scope = getScope(cx);

    ObjectWrapper o(cx, scope->getProto<MaxKeyInfo>().getProto());

    JS::RootedValue val(cx);

    if (!o.hasField(InternedString::singleton)) {
        JS::RootedObject thisv(cx);
        scope->getProto<MaxKeyInfo>().newObject(&thisv);

        val.setObjectOrNull(thisv);
        o.setValue(InternedString::singleton, val);
    } else {
        o.getValue(InternedString::singleton, &val);

        if (!getScope(cx)->getProto<MaxKeyInfo>().instanceOf(val))
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

    auto scope = getScope(cx);

    args.rval().setBoolean(scope->getProto<MaxKeyInfo>().instanceOf(args.get(0)));
}

void MaxKeyInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    ObjectWrapper protoWrapper(cx, proto);

    JS::RootedValue value(cx);
    getScope(cx)->getProto<MaxKeyInfo>().newObject(&value);

    ObjectWrapper(cx, global).setValue(InternedString::MaxKey, value);
    protoWrapper.setValue(InternedString::singleton, value);
}

}  // namespace mozjs
}  // namespace mongo

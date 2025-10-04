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

#include "mongo/scripting/mozjs/code.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec CodeInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, CodeInfo),
    JS_FS_END,
};

const char* const CodeInfo::className = "Code";

void CodeInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    std::string str = str::stream()
        << "Code({\"code\":\"" << o.getString(InternedString::code) << "\","
        << "\"scope\":" << o.getObject(InternedString::scope) << "\"})";

    ValueReader(cx, args.rval()).fromStringData(str);
}

void CodeInfo::construct(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue,
            "Code needs 0, 1 or 2 arguments",
            args.length() == 0 || args.length() == 1 || args.length() == 2);
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);

    scope->getProto<CodeInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    if (args.length() == 0) {
        o.setString(InternedString::code, "");
    } else if (args.length() == 1) {
        JS::HandleValue codeArg = args.get(0);
        if (!codeArg.isString())
            uasserted(ErrorCodes::BadValue, "code must be a string");

        o.setValue(InternedString::code, codeArg);
    } else {
        if (!args.get(0).isString())
            uasserted(ErrorCodes::BadValue, "code must be a string");
        if (!args.get(1).isObject())
            uasserted(ErrorCodes::BadValue, "scope must be an object");

        o.setValue(InternedString::code, args.get(0));
        o.setValue(InternedString::scope, args.get(1));
    }

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/code.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
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
    auto* runtime = getCommonRuntime(cx);

    JS::RootedObject thisv(cx);

    getProto<CodeInfo>(runtime).newObject(&thisv);
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
